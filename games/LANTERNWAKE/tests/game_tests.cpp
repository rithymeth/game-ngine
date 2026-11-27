#include "game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace lanternwake;
int failures = 0;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); ++failures; \
} } while (false)

bool Close(float a, float b, float epsilon = 0.025f) { return std::fabs(a - b) <= epsilon; }
float Distance(Position a, Position b) { return std::hypot(a.x - b.x, a.y - b.y); }

Keeper& Player(Game& game) {
    Keeper* keeper = nullptr;
    game.World().ForEach<Keeper>([&](Keeper& value) { keeper = &value; });
    return *keeper;
}

void PlacePlayer(Game& game, Position target) {
    game.World().ForEach<Position, Keeper>([&](Position& p, Keeper&) { p = target; });
}

void QuietShades(Game& game) {
    game.World().ForEach<Wraith>([](Wraith& w) { w.stunned = 100000.0f; });
}

void OverlapShades(Game& game) {
    const Position target = game.PlayerPosition();
    game.World().ForEach<Position, Wraith>([&](Position& p, Wraith& w) {
        p = target;
        w.stunned = 0;
    });
}

void Begin(Game& game, bool quiet = true) {
    game.Step(Input{.start = true});
    CHECK(game.State() == Phase::Playing);
    if (quiet) QuietShades(game);
}

void Advance(Game& game, int steps, const Input& input = {}) {
    for (int i = 0; i < steps; ++i) game.Step(input);
}

int Events(const Game& game, EventType type) {
    return static_cast<int>(std::count_if(game.Events().begin(), game.Events().end(),
        [=](const Event& event) { return event.type == type; }));
}

void CheckEmbers(const Game& game) {
    int picked = 0;
    game.World().ForEach<Mote>([&](Mote& mote) { picked += mote.collected ? 1 : 0; });
    CHECK(picked == game.Collected());
    CHECK(game.Player().carried + 4 * game.LitShrines() == game.Collected());
    CHECK(game.Player().carried >= 0);
    CHECK(game.Collected() <= 12);
}

void Collect(Game& game, int begin, int end) {
    for (int i = begin; i < end; ++i) {
        PlacePlayer(game, Game::MotePositions()[i]);
        game.Step({});
        CHECK(Events(game, EventType::Pickup) == 1);
        CheckEmbers(game);
    }
}

void LayoutAndTitle() {
    Game game;
    CHECK(game.State() == Phase::Title);
    CHECK(game.World().EntityCount() == 22);
    CHECK(game.Player().health == 5);
    CHECK(Close(game.PlayerPosition().x, 640) && Close(game.PlayerPosition().y, 650));
    CHECK(game.Collected() == 0 && game.LitShrines() == 0);
    for (const Position p : Game::MotePositions()) {
        CHECK(p.x >= 150 && p.x <= 1130 && p.y >= 210 && p.y <= 690);
        for (const Position shrine : Game::ShrinePositions()) CHECK(Distance(p, shrine) >= 65);
    }
    game.World().ForEach<Position, Wraith>([&](Position& p, Wraith&) {
        CHECK(Distance(p, game.PlayerPosition()) > 180);
    });
    game.Step(Input{.move_x = 1, .restart = true, .dash = true, .pulse = true});
    CHECK(game.State() == Phase::Title && game.Time() == 0 && game.Events().empty());
    Begin(game);
    CHECK(Events(game, EventType::Start) == 1 && game.Time() == 0);
    game.Step(Input{.restart = true});
    CHECK(game.Time() > 0 && game.Events().empty());
}

void MovementAndBounds() {
    Game horizontal, diagonal, analog;
    for (Game* game : {&horizontal, &diagonal, &analog}) {
        Begin(*game);
        PlacePlayer(*game, {640, 400});
    }
    Advance(horizontal, 60, Input{.move_x = 1});
    Advance(diagonal, 60, Input{.move_x = 1, .move_y = 1});
    Advance(analog, 60, Input{.move_x = 0.25f});
    CHECK(Close(Distance({640, 400}, horizontal.PlayerPosition()), 170));
    CHECK(Close(Distance({640, 400}, diagonal.PlayerPosition()), 170));
    CHECK(Close(Distance({640, 400}, analog.PlayerPosition()), 42.5f));
    CHECK(Close(diagonal.Player().facing_x, 0.7071068f));
    CHECK(Close(diagonal.Player().facing_y, 0.7071068f));

    PlacePlayer(horizontal, {120, 400});
    Advance(horizontal, 60, Input{.move_x = -1});
    CHECK(Close(horizontal.PlayerPosition().x, 115));
    PlacePlayer(horizontal, {580, 200});
    Advance(horizontal, 60, Input{.move_y = -1});
    CHECK(Close(horizontal.PlayerPosition().y, 195));
    PlacePlayer(horizontal, {640, 200});
    Advance(horizontal, 60, Input{.move_y = -1});
    CHECK(Close(horizontal.PlayerPosition().y, 150));
    PlacePlayer(horizontal, {640, 164});
    Advance(horizontal, 60, Input{.move_x = -1});
    CHECK(Close(horizontal.PlayerPosition().x, 590));
    CHECK(Close(horizontal.PlayerPosition().y, 164));
    Advance(horizontal, 60, Input{.move_x = 1});
    CHECK(Close(horizontal.PlayerPosition().x, 690));
    Advance(horizontal, 300, Input{.move_y = 1});
    CHECK(Close(horizontal.PlayerPosition().y, 715));
    const Position before = horizontal.PlayerPosition();
    horizontal.Step(Input{.move_x = std::numeric_limits<float>::quiet_NaN(),
                          .move_y = std::numeric_limits<float>::infinity()});
    CHECK(Close(Distance(before, horizontal.PlayerPosition()), 0));
}

void DashTimingAndProtection() {
    Game game;
    Begin(game);
    PlacePlayer(game, {640, 400});
    Player(game).facing_x = 1;
    Player(game).facing_y = 0;
    game.Step(Input{.dash = true});
    CHECK(Events(game, EventType::Dash) == 1);
    CHECK(Close(game.Player().energy, 100));
    game.Step(Input{.dash = true});
    CHECK(Events(game, EventType::Dash) == 0);
    Advance(game, 9);
    CHECK(Close(game.PlayerPosition().x, 730)); // 0.18 seconds at 500 px/s.
    CHECK(game.Player().dash_time == 0);
    game.Step(Input{.dash = true});
    CHECK(Events(game, EventType::Dash) == 0);
    Advance(game, 60);
    game.Step(Input{.dash = true});
    CHECK(Events(game, EventType::Dash) == 1);

    Game protected_game;
    Begin(protected_game);
    PlacePlayer(protected_game, {640, 400});
    OverlapShades(protected_game);
    protected_game.Step(Input{.move_x = 1, .dash = true});
    CHECK(protected_game.Player().health == 5);
    const float dash_y = protected_game.PlayerPosition().y;
    protected_game.Step(Input{.move_y = 1});
    CHECK(Close(protected_game.PlayerPosition().y, dash_y));
    CHECK(Close(protected_game.Player().facing_x, 1));
    CHECK(Close(protected_game.Player().facing_y, 0));
}

void PulseCostCooldownAndStun() {
    Game game;
    Begin(game);
    PlacePlayer(game, {640, 400});
    Position* target_position = nullptr;
    Wraith* target = nullptr;
    game.World().ForEach<Position, Wraith>([&](Position& p, Wraith& w) {
        if (!target) { target = &w; target_position = &p; p = {760, 400}; w.stunned = 0; }
    });
    game.Step(Input{.pulse = true});
    CHECK(Events(game, EventType::Pulse) == 1);
    CHECK(Close(game.Player().energy, 72));
    CHECK(game.Player().pulse_age == 0);
    CHECK(target->stunned >= 2.28f && target->stunned <= 2.3f);
    CHECK(target_position->x > 790);
    const Position stunned_position = *target_position;
    game.Step(Input{.pulse = true});
    CHECK(Events(game, EventType::Pulse) == 0);
    CHECK(game.Player().energy > 72 && game.Player().energy < 73);
    Advance(game, 59);
    CHECK(Close(Distance(*target_position, stunned_position), 0));
    CHECK(target->stunned > 1.2f);

    Player(game).pulse_cooldown = 0;
    Player(game).energy = 27;
    game.Step(Input{.pulse = true});
    CHECK(Events(game, EventType::Pulse) == 0);
    CHECK(game.Player().energy >= 27 && game.Player().energy < 28);
    Player(game).energy = 28;
    game.Step(Input{.pulse = true});
    CHECK(Events(game, EventType::Pulse) == 1 && game.Player().energy >= 0);

    *target_position = game.PlayerPosition();
    target->stunned = 1;
    const int health = game.Player().health;
    game.Step({});
    CHECK(game.Player().health == health); // A stunned shade cannot hurt the keeper.
}

void PickupsArePermanent() {
    Game game;
    Begin(game);
    Collect(game, 0, 12);
    CHECK(game.Collected() == 12 && game.Player().carried == 12);
    game.Step({});
    CHECK(Events(game, EventType::Pickup) == 0);
    CHECK(game.World().EntityCount() == 22);
    CheckEmbers(game);
}

void ShrineOfferingIsAtomic() {
    Game game;
    Begin(game);
    Collect(game, 0, 4);
    PlacePlayer(game, Game::ShrinePositions()[0]);
    CHECK(game.NearbyShrine() == 0);
    Advance(game, 47, Input{.interact = true});
    CHECK(game.LitShrines() == 0 && game.Player().carried == 4);
    CHECK(game.ChannelProgress() > 0.97f && game.ChannelProgress() < 1);
    game.Step(Input{.interact = true});
    CHECK(game.LitShrines() == 1 && game.Player().carried == 0);
    CHECK(Events(game, EventType::ShrineLit) == 1);
    CheckEmbers(game);
    Advance(game, 80, Input{.interact = true});
    CHECK(game.LitShrines() == 1 && Events(game, EventType::ShrineLit) == 0);
    PlacePlayer(game, Game::ShrinePositions()[1]);
    Advance(game, 60, Input{.interact = true});
    CHECK(game.LitShrines() == 1 && game.ChannelProgress() == 0);
    CheckEmbers(game);
}

void WalkingReleaseAndDistanceInterruptOffering() {
    Game game;
    Begin(game);
    Collect(game, 0, 4);
    PlacePlayer(game, Game::ShrinePositions()[0]);
    Advance(game, 24, Input{.interact = true});
    CHECK(Close(game.ChannelProgress(), 0.5f));
    game.Step(Input{.move_x = 0.5f, .interact = true});
    CHECK(game.ChannelProgress() == 0 && game.Player().carried == 4);
    Advance(game, 24, Input{.interact = true});
    game.Step({});
    CHECK(game.ChannelProgress() == 0);
    Advance(game, 24, Input{.interact = true});
    PlacePlayer(game, {640, 650});
    game.Step(Input{.interact = true});
    CHECK(game.ChannelProgress() == 0 && game.Player().carried == 4);
    PlacePlayer(game, Game::ShrinePositions()[0]);
    Advance(game, 48, Input{.interact = true});
    CHECK(game.LitShrines() == 1);
    CheckEmbers(game);
}

void CompleteJourneyAndReplay() {
    Game game;
    Begin(game);
    Collect(game, 0, 12);
    for (int shrine = 0; shrine < 3; ++shrine) {
        PlacePlayer(game, Game::ShrinePositions()[shrine]);
        Advance(game, 48, Input{.interact = true});
        CHECK(game.LitShrines() == shrine + 1);
        if (shrine == 2) {
            for (const Event& event : game.Events())
                if (event.type == EventType::ShrineLit) CHECK(event.value == 3);
        }
        CheckEmbers(game);
    }
    CHECK(game.State() == Phase::Playing && !game.NearGate());
    PlacePlayer(game, Game::GatePosition());
    game.Step({});
    CHECK(game.NearGate() && game.State() == Phase::Playing);
    game.Step(Input{.interact = true});
    CHECK(game.State() == Phase::Won && Events(game, EventType::Victory) == 1);
    const float time = game.Time(), energy = game.Player().energy;
    const Position position = game.PlayerPosition();
    const int score = game.Score();
    Advance(game, 60, Input{.move_x = 1, .interact = true, .start = true, .dash = true, .pulse = true});
    CHECK(game.State() == Phase::Won && game.Time() == time && game.Player().energy == energy);
    CHECK(Distance(game.PlayerPosition(), position) == 0 && game.Score() == score && game.Events().empty());
    game.Step(Input{.restart = true});
    CHECK(game.State() == Phase::Playing && game.Time() == 0 && game.Player().health == 5);
    CHECK(game.Collected() == 0 && game.LitShrines() == 0 && game.Player().carried == 0);
    CHECK(game.Hits() == 0 && Events(game, EventType::Start) == 1);
    CHECK(game.World().EntityCount() == 22);
    CheckEmbers(game);
}

void RestoredGroveHasHarmlessShades() {
    Game game;
    Begin(game);
    Collect(game, 0, 12);
    for (const Position shrine : Game::ShrinePositions()) {
        PlacePlayer(game, shrine);
        Advance(game, 48, Input{.interact = true});
    }
    CHECK(game.LitShrines() == 3);
    const int health = game.Player().health, hits = game.Hits();
    for (int frame = 0; frame < 120; ++frame) {
        OverlapShades(game);
        game.Step({});
    }
    CHECK(game.State() == Phase::Playing);
    CHECK(game.Player().health == health && game.Hits() == hits);
    CHECK(Events(game, EventType::Hurt) == 0);
}

void CompleteJourneyThroughRealMovement() {
    Game game;
    Begin(game, false);
    int leg = 0, frames = 0;
    while (game.State() == Phase::Playing && frames++ < 20000) {
        const Position target = leg < 12 ? Game::MotePositions()[leg]
            : leg < 15 ? Game::ShrinePositions()[leg - 12] : Game::GatePosition();
        const Position player = game.PlayerPosition();
        const float dx = target.x - player.x, dy = target.y - player.y;
        const float distance = std::hypot(dx, dy);
        Input input;
        if (distance >= (leg < 12 ? 20.0f : 55.0f)) {
            input.move_x = dx / distance;
            input.move_y = dy / distance;
        } else if (leg >= 12) {
            input.interact = true;
        }
        float danger = 10000;
        game.World().ForEach<Position, Wraith>([&](Position& p, Wraith& w) {
            if (w.stunned == 0) danger = std::min(danger, Distance(p, player));
        });
        input.pulse = danger < 145 && game.Player().energy >= 28 && game.Player().pulse_cooldown == 0;
        input.dash = distance > 120 && game.Player().dash_cooldown == 0;
        game.Step(input);
        if (leg < 12) {
            bool collected = false;
            game.World().ForEach<Mote>([&](Mote& mote) {
                if (mote.index == leg) collected = mote.collected;
            });
            if (collected) ++leg;
        } else if (leg < 15 && game.LitShrines() > leg - 12) {
            ++leg;
        }
        CheckEmbers(game);
    }
    CHECK(game.State() == Phase::Won);
    CHECK(game.Collected() == 12 && game.LitShrines() == 3 && game.Player().carried == 0);
    CHECK(game.Player().health > 0);
    std::printf("  Real-input route: %d frames, %.2fs, %d health, %d hits, score %d\n",
                frames, game.Time(), game.Player().health, game.Hits(), game.Score());
}

void DamageInvulnerabilityAndDefeat() {
    Game game;
    Begin(game, false);
    OverlapShades(game);
    game.Step({});
    CHECK(game.Player().health == 4 && game.Hits() == 1);
    CHECK(Events(game, EventType::Hurt) == 1);
    for (int i = 0; i < 82; ++i) { OverlapShades(game); game.Step({}); }
    CHECK(game.Player().health == 4 && game.Hits() == 1);
    int frames = 0;
    while (game.State() == Phase::Playing && frames++ < 600) {
        OverlapShades(game);
        game.Step({});
    }
    CHECK(game.State() == Phase::Lost && game.Player().health == 0 && game.Hits() == 5);
    CHECK(Events(game, EventType::Defeat) == 1 && Events(game, EventType::Hurt) == 1);
    const float time = game.Time();
    game.Step(Input{.move_x = 1, .dash = true, .pulse = true});
    CHECK(game.Time() == time && game.State() == Phase::Lost && game.Events().empty());
    game.Step(Input{.restart = true});
    CHECK(game.State() == Phase::Playing && game.Player().health == 5 && game.Hits() == 0);
}

void PauseFreezesAndRestartIsRestricted() {
    Game game;
    Begin(game);
    game.Step(Input{.pulse = true});
    const float time = game.Time(), energy = game.Player().energy, cooldown = game.Player().pulse_cooldown;
    const Position position = game.PlayerPosition();
    game.Step(Input{.pause = true});
    CHECK(game.State() == Phase::Paused && game.Events().empty());
    Advance(game, 120, Input{.move_x = 1, .interact = true, .start = true, .dash = true, .pulse = true});
    CHECK(game.Time() == time && game.Player().energy == energy && game.Player().pulse_cooldown == cooldown);
    CHECK(Distance(game.PlayerPosition(), position) == 0);
    game.Step(Input{.pause = true});
    CHECK(game.State() == Phase::Playing && game.Time() == time);
    game.Step(Input{.restart = true});
    CHECK(game.Time() > time && game.Player().energy < 100);
    game.Step(Input{.pause = true});
    game.Step(Input{.restart = true});
    CHECK(game.State() == Phase::Playing && game.Time() == 0 && game.Player().energy == 100);
}

void GateNeedsAllShrinesAndScoreNeverGoesNegative() {
    Game game;
    Begin(game);
    PlacePlayer(game, Game::GatePosition());
    game.Step(Input{.interact = true});
    CHECK(game.State() == Phase::Playing);
    const int score = game.Score();
    Advance(game, 120);
    CHECK(game.Score() < score);
    Advance(game, 12000);
    CHECK(game.Score() == 0);
}

void ReplayIsDeterministic() {
    Game first, second;
    Begin(first, false);
    Begin(second, false);
    bool matches = true;
    for (int frame = 0; frame < 1800; ++frame) {
        const int leg = (frame / 120) % 4;
        Input input;
        input.move_x = leg == 0 ? -1.0f : leg == 2 ? 1.0f : 0.0f;
        input.move_y = leg == 1 ? -1.0f : leg == 3 ? 1.0f : 0.0f;
        input.dash = frame % 113 == 0;
        input.pulse = frame % 197 == 0;
        input.interact = frame % 251 < 55;
        first.Step(input);
        second.Step(input);
        matches = matches && first.State() == second.State() && first.Time() == second.Time()
            && first.PlayerPosition().x == second.PlayerPosition().x
            && first.PlayerPosition().y == second.PlayerPosition().y
            && first.Player().energy == second.Player().energy
            && first.Player().health == second.Player().health && first.Score() == second.Score()
            && first.Events().size() == second.Events().size();
        CheckEmbers(first);
    }
    CHECK(matches);
}

} // namespace

int main() {
    const struct { const char* name; void (*run)(); } tests[] = {
        {"layout and title", LayoutAndTitle},
        {"normalized movement and boundaries", MovementAndBounds},
        {"dash timing and invulnerability", DashTimingAndProtection},
        {"pulse cost, cooldown and stun", PulseCostCooldownAndStun},
        {"permanent pickups and ember conservation", PickupsArePermanent},
        {"atomic shrine offerings", ShrineOfferingIsAtomic},
        {"offering interruptions", WalkingReleaseAndDistanceInterruptOffering},
        {"complete journey, terminal freeze and replay", CompleteJourneyAndReplay},
        {"restored grove has harmless shades", RestoredGroveHasHarmlessShades},
        {"complete journey through real movement", CompleteJourneyThroughRealMovement},
        {"contact damage, invulnerability and defeat", DamageInvulnerabilityAndDefeat},
        {"pause freeze and restart rules", PauseFreezesAndRestartIsRestricted},
        {"locked gate and score floor", GateNeedsAllShrinesAndScoreNeverGoesNegative},
        {"deterministic replay", ReplayIsDeterministic},
    };
    for (const auto& test : tests) {
        const int before = failures;
        test.run();
        std::printf("[%s] %s\n", before == failures ? "PASS" : "FAIL", test.name);
    }
    std::printf("Lanternwake: %zu tests, %d failures\n", sizeof(tests) / sizeof(tests[0]), failures);
    return failures ? 1 : 0;
}
