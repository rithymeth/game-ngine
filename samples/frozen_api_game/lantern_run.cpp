#include "lantern_run.h"

#include "aether/pak/pak.h"

#include <cmath>

namespace lantern {

using namespace aether;

Game::Game(const Settings& settings) : settings_(settings) {
    player_ = world_.CreateEntity(Lamplighter{Vec3(0, 0, 0), 0});
    for (i32 i = 0; i < settings_.lantern_count; ++i)
        world_.CreateEntity(Lantern{Vec3(static_cast<f32>(i + 1) * settings_.spacing, 0, 0), false});
}

void Game::Step(f32 dt) {
    Lamplighter* p = world_.GetComponent<Lamplighter>(player_);
    p->position = p->position + Vec3(settings_.walk_speed * dt, 0, 0);
    i32 lit = 0;
    world_.ForEach<Lantern>([&](Lantern& l) {
        if (!l.lit && (l.position - p->position).Length() <= settings_.reach) l.lit = true;
        lit += l.lit ? 1 : 0;
    });
    p->lit = lit;
}

i32 Game::Lit() const { return world_.GetComponent<Lamplighter>(player_)->lit; }
f32 Game::PlayerX() const { return world_.GetComponent<Lamplighter>(player_)->position.x; }

std::string Game::Save() const {
    std::string out = "{\"x\":" + std::to_string(PlayerX()) + ",\"lit\":[";
    bool first = true;
    world_.ForEach<Lantern>([&](Lantern& l) {
        out += (first ? "" : ",") + std::string(l.lit ? "1" : "0");
        first = false;
    });
    return out + "]}";
}

bool Game::Load(const std::string& json) {
    const reflect::Json doc = reflect::Json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("x") || !doc.contains("lit") || !doc["lit"].is_array()) return false;
    world_.GetComponent<Lamplighter>(player_)->position.x = doc["x"].get<f32>();
    usize i = 0;
    bool ok = true;
    world_.ForEach<Lantern>([&](Lantern& l) {
        if (i < doc["lit"].size()) l.lit = doc["lit"][i].get<int>() != 0;
        else ok = false;
        ++i;
    });
    Step(0.0f); // recounts what is lit
    return ok;
}

bool ParseSettings(const std::string& json, Settings& out) { return reflect::LoadJsonText(reflect::Reflect<Settings>(), &out, json); }

std::vector<u8> PackSettings(const Settings& settings) {
    pak::PakWriter writer;
    writer.Add("config/settings.json", reflect::SaveJsonText(reflect::Reflect<Settings>(), &settings));
    return writer.Build();
}

bool UnpackSettings(const std::vector<u8>& archive, Settings& out) {
    pak::PakReader reader;
    std::string text;
    return reader.OpenMemory(archive) && reader.ReadText("config/settings.json", text) && ParseSettings(text, out);
}

} // namespace lantern
