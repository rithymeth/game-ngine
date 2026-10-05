#include "aether/save/save_games.h"

#include <algorithm>

namespace aether::save {

namespace {

SaveSystem* S() { return SaveSystem::Active(); }

bool Report(SaveSystem& system, const SaveResult& r) {
    system.SetLastError(r.ok ? std::string() : r.message);
    return r.ok;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        if (!out.empty()) out += "; ";
        out += l;
    }
    return out;
}

} // namespace

void SaveGames::CreateSaveObject() {
    if (SaveSystem* s = S()) s->Bag().Clear();
}

void SaveGames::SetBool(const std::string& key, bool value) {
    if (SaveSystem* s = S()) s->Bag().SetBool(key, value);
}
void SaveGames::SetInt(const std::string& key, i32 value) {
    if (SaveSystem* s = S()) s->Bag().SetInt(key, value);
}
void SaveGames::SetFloat(const std::string& key, f32 value) {
    if (SaveSystem* s = S()) s->Bag().SetFloat(key, value);
}
void SaveGames::SetString(const std::string& key, const std::string& value) {
    if (SaveSystem* s = S()) s->Bag().SetString(key, value);
}
void SaveGames::SetVector(const std::string& key, const Vec3& value) {
    if (SaveSystem* s = S()) s->Bag().SetVector(key, value);
}
bool SaveGames::GetBool(const std::string& key, bool fallback) {
    SaveSystem* s = S();
    return s ? s->Bag().GetBool(key, fallback) : fallback;
}
i32 SaveGames::GetInt(const std::string& key, i32 fallback) {
    SaveSystem* s = S();
    return s ? s->Bag().GetInt(key, fallback) : fallback;
}
f32 SaveGames::GetFloat(const std::string& key, f32 fallback) {
    SaveSystem* s = S();
    return s ? s->Bag().GetFloat(key, fallback) : fallback;
}
std::string SaveGames::GetString(const std::string& key, const std::string& fallback) {
    SaveSystem* s = S();
    return s ? s->Bag().GetString(key, fallback) : fallback;
}
Vec3 SaveGames::GetVector(const std::string& key, const Vec3& fallback) {
    SaveSystem* s = S();
    return s ? s->Bag().GetVector(key, fallback) : fallback;
}
bool SaveGames::HasKey(const std::string& key) {
    SaveSystem* s = S();
    return s && s->Bag().Has(key);
}
bool SaveGames::RemoveKey(const std::string& key) {
    SaveSystem* s = S();
    return s && s->Bag().Remove(key);
}

bool SaveGames::SaveToSlot(const std::string& slot) {
    SaveSystem* s = S();
    if (!s) return false;
    return Report(*s, s->Save(slot, s->Bag()));
}

void SaveGames::SaveToSlotAsync(const std::string& slot) {
    SaveSystem* s = S();
    if (!s) return;
    SaveSystem* system = s;
    s->SaveAsync(slot, s->Bag(), [system, slot](const SaveResult& r) {
        if (!r.ok) system->SetLastError(r.message);
        system->NoteFinishedSave({slot, r.ok});
    });
}

bool SaveGames::LoadFromSlot(const std::string& slot) {
    SaveSystem* s = S();
    if (!s) return false;
    SaveBag loaded;
    const SaveResult r = s->Load(slot, loaded);
    if (r.ok) s->Bag() = std::move(loaded);
    return Report(*s, r);
}

bool SaveGames::DoesSaveExist(const std::string& slot) {
    SaveSystem* s = S();
    return s && s->Exists(slot);
}

bool SaveGames::DeleteSave(const std::string& slot) {
    SaveSystem* s = S();
    return s && Report(*s, s->DeleteSlot(slot));
}

i32 SaveGames::GetSlotCount() {
    SaveSystem* s = S();
    return s ? static_cast<i32>(s->ListSlots().size()) : 0;
}

std::string SaveGames::GetSlotName(i32 index) {
    SaveSystem* s = S();
    if (!s || index < 0) return {};
    const std::vector<SlotInfo> slots = s->ListSlots();
    return static_cast<usize>(index) < slots.size() ? slots[static_cast<usize>(index)].slot : std::string();
}

std::string SaveGames::GetLastError() {
    SaveSystem* s = S();
    return s ? s->LastError() : std::string();
}

bool SaveGames::CaptureWorld() {
    SaveSystem* s = S();
    if (!s) return false;
    const SaveSystem::WorldContext& c = s->GetWorldContext();
    if (!c.world || !c.guids) {
        s->SetLastError("no world to capture: the host hasn't given the save system one");
        return false;
    }
    CaptureReport report;
    s->Bag().world = save::CaptureWorld(*c.world, *c.guids, c.tracker, &report);
    s->Bag().has_world = true;
    s->SetLastError(Join(report.warnings));
    return true;
}

bool SaveGames::RestoreWorld() {
    SaveSystem* s = S();
    if (!s) return false;
    const SaveSystem::WorldContext& c = s->GetWorldContext();
    if (!c.world || !c.guids) {
        s->SetLastError("no world to restore into: the host hasn't given the save system one");
        return false;
    }
    if (!s->Bag().has_world) {
        s->SetLastError("this save has no world state (Capture World wasn't called before saving)");
        return false;
    }
    RestoreReport report;
    const bool ok = save::RestoreWorld(*c.world, *c.guids, s->Bag().world, &report, c.lifecycle);
    std::vector<std::string> problems = report.warnings;
    if (!report.missing.empty()) problems.push_back(std::to_string(report.missing.size()) + " saved entities aren't in the scene");
    for (const std::string& u : report.unknown_components) problems.push_back("unknown component " + u);
    for (const std::string& u : report.unknown_fields) problems.push_back("unknown field " + u);
    s->SetLastError(Join(problems));
    return ok;
}

} // namespace aether::save
