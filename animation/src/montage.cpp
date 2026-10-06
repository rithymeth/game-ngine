#include "aether/animation/montage.h"
#include "aether/core/json_util.h"

#include <algorithm>
#include <set>

namespace aether::anim {

using nlohmann::json;

namespace {
constexpr int kFormatVersion = 1;
}

i32 Montage::FindSection(const std::string& n) const {
    for (usize i = 0; i < sections.size(); ++i) {
        if (sections[i].name == n) return static_cast<i32>(i);
    }
    return -1;
}

i32 Montage::SectionAt(f32 time) const {
    i32 found = sections.empty() ? -1 : 0;
    for (usize i = 0; i < sections.size(); ++i) {
        if (sections[i].start <= time) found = static_cast<i32>(i);
    }
    return found;
}

f32 Montage::SectionEnd(i32 section, f32 clip_duration) const {
    if (section < 0 || static_cast<usize>(section) + 1 >= sections.size()) return clip_duration;
    return sections[static_cast<usize>(section) + 1].start;
}

void Montage::Normalize() {
    std::stable_sort(sections.begin(), sections.end(), [](const MontageSection& a, const MontageSection& b) { return a.start < b.start; });
}

std::vector<std::string> ValidateMontage(const Montage& m) {
    std::vector<std::string> out;
    if (m.clip.empty()) out.push_back("MN001: the montage '" + m.name + "' has no clip.");
    std::set<std::string> names;
    std::set<f32> starts;
    for (const MontageSection& s : m.sections) {
        if (!names.insert(s.name).second || !starts.insert(s.start).second) {
            out.push_back("MN003: two sections of '" + m.name + "' share the name or start of '" + s.name + "'.");
        }
        if (!s.next.empty() && m.FindSection(s.next) < 0) {
            out.push_back("MN002: the section '" + s.name + "' goes on to '" + s.next + "', which isn't a section.");
        }
    }
    if (m.blend_in < 0.0f || m.blend_out < 0.0f) out.push_back("MN004: the montage '" + m.name + "' has a negative blend time.");
    return out;
}

json MontageToJson(const Montage& m) {
    json sections = json::array();
    for (const MontageSection& s : m.sections) {
        json j = {{"name", s.name}, {"start", s.start}};
        if (!s.next.empty()) j["next"] = s.next;
        sections.push_back(std::move(j));
    }
    return {{"$type", "Montage"}, {"$version", kFormatVersion}, {"name", m.name},   {"clip", m.clip},
            {"slot", m.slot},     {"blend_in", m.blend_in},     {"blend_out", m.blend_out}, {"sections", sections}};
}

static bool MontageFromJsonImpl(const json& j, Montage& out, std::string* error);

bool MontageFromJson(const json& j, Montage& out, std::string* error) {
    return GuardJsonLoader(error, "montage", [&] { return MontageFromJsonImpl(j, out, error); });
}

static bool MontageFromJsonImpl(const json& j, Montage& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "Montage") return fail("not a montage file");
    if (JsonVersion(j) > kFormatVersion) return fail("saved by a newer version of the engine");
    Montage m;
    m.name = j.value("name", "");
    m.clip = j.value("clip", "");
    m.slot = j.value("slot", "Default");
    m.blend_in = j.value("blend_in", 0.2f);
    m.blend_out = j.value("blend_out", 0.2f);
    for (const json& s : j.value("sections", json::array())) {
        if (!s.is_object() || !s.value("name", json()).is_string() || !s.value("start", json()).is_number()) {
            return fail("a section needs a name and a start");
        }
        m.sections.push_back({s["name"].get<std::string>(), s["start"].get<f32>(), s.value("next", "")});
    }
    m.Normalize();
    out = std::move(m);
    return true;
}

} // namespace aether::anim
