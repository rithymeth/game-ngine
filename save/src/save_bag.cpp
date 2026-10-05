#include "aether/save/save_bag.h"

namespace aether::save {

const SaveEntry* SaveBag::Find(const std::string& key) const {
    for (const SaveEntry& e : entries) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

bool SaveBag::Remove(const std::string& key) {
    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].key == key) {
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void SaveBag::Clear() {
    entries.clear();
    world = WorldSnapshot{};
    has_world = false;
}

SaveEntry& SaveBag::Slot(const std::string& key, u8 kind) {
    for (SaveEntry& e : entries) {
        if (e.key == key) {
            e = SaveEntry{}; // a new value of any kind replaces the old one entirely
            e.key = key;
            e.kind = kind;
            return e;
        }
    }
    SaveEntry e;
    e.key = key;
    e.kind = kind;
    entries.push_back(std::move(e));
    return entries.back();
}

void SaveBag::SetBool(const std::string& key, bool value) { Slot(key, SaveEntry::Bool).b = value; }
void SaveBag::SetInt(const std::string& key, i32 value) { Slot(key, SaveEntry::Int).i = value; }
void SaveBag::SetFloat(const std::string& key, f32 value) { Slot(key, SaveEntry::Float).f = value; }
void SaveBag::SetString(const std::string& key, const std::string& value) { Slot(key, SaveEntry::String).s = value; }
void SaveBag::SetVector(const std::string& key, const Vec3& value) { Slot(key, SaveEntry::Vector).v = value; }

bool SaveBag::GetBool(const std::string& key, bool fallback) const {
    const SaveEntry* e = Find(key);
    return e && e->kind == SaveEntry::Bool ? e->b : fallback;
}
i32 SaveBag::GetInt(const std::string& key, i32 fallback) const {
    const SaveEntry* e = Find(key);
    return e && e->kind == SaveEntry::Int ? e->i : fallback;
}
f32 SaveBag::GetFloat(const std::string& key, f32 fallback) const {
    const SaveEntry* e = Find(key);
    return e && e->kind == SaveEntry::Float ? e->f : fallback;
}
std::string SaveBag::GetString(const std::string& key, const std::string& fallback) const {
    const SaveEntry* e = Find(key);
    return e && e->kind == SaveEntry::String ? e->s : fallback;
}
Vec3 SaveBag::GetVector(const std::string& key, const Vec3& fallback) const {
    const SaveEntry* e = Find(key);
    return e && e->kind == SaveEntry::Vector ? e->v : fallback;
}

} // namespace aether::save
