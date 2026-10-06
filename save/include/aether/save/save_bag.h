#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"
#include "aether/save/world_state.h"

#include <string>
#include <vector>

// A save without a C++ struct (Phase 28 step 4, docs/design/PHASE_SPECS.md
// §28.4): a bag of named values (bool, int, float, string, vector) and,
// optionally, a world snapshot. Blueprints and Luau can't name a game's own
// reflected struct, so they keep their save state in the bag; it is itself an
// ordinary reflected struct, so game code can save and load it with
// SaveSystem too and the two share slots. A bag has no schema: nothing checks
// that a key keeps its type from one version of the game to the next, so
// Blueprint saves are loosely typed (a get of the wrong kind gives its
// default).

namespace aether::save {

struct SaveEntry {
    enum Kind : u8 { None = 0, Bool = 1, Int = 2, Float = 3, String = 4, Vector = 5 };
    std::string key;
    u8 kind = None;
    bool b = false;
    i32 i = 0;
    f32 f = 0.0f;
    std::string s;
    Vec3 v{0.0f, 0.0f, 0.0f};
};

struct SaveBag {
    std::vector<SaveEntry> entries; // keys are unique; lookup is a linear scan (bags are small)
    WorldSnapshot world;
    bool has_world = false;

    const SaveEntry* Find(const std::string& key) const;
    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    bool Remove(const std::string& key);
    void Clear();

    // Set replaces an entry of the same key (whatever its kind was).
    void SetBool(const std::string& key, bool value);
    void SetInt(const std::string& key, i32 value);
    void SetFloat(const std::string& key, f32 value);
    void SetString(const std::string& key, const std::string& value);
    void SetVector(const std::string& key, const Vec3& value);

    // A missing key, or one of another kind, gives `fallback`.
    bool GetBool(const std::string& key, bool fallback = false) const;
    i32 GetInt(const std::string& key, i32 fallback = 0) const;
    f32 GetFloat(const std::string& key, f32 fallback = 0.0f) const;
    std::string GetString(const std::string& key, const std::string& fallback = {}) const;
    Vec3 GetVector(const std::string& key, const Vec3& fallback = {}) const;

private:
    SaveEntry& Slot(const std::string& key, u8 kind);
};

} // namespace aether::save

AETHER_REFLECT(aether::save::SaveEntry, 1, AETHER_FIELD(key, Field_EditAnywhere), AETHER_FIELD(kind, Field_EditAnywhere),
               AETHER_FIELD(b, Field_EditAnywhere), AETHER_FIELD(i, Field_EditAnywhere), AETHER_FIELD(f, Field_EditAnywhere),
               AETHER_FIELD(s, Field_EditAnywhere), AETHER_FIELD(v, Field_EditAnywhere))
AETHER_REFLECT(aether::save::SaveBag, 1, AETHER_FIELD(entries, Field_EditAnywhere), AETHER_FIELD(world, Field_EditAnywhere),
               AETHER_FIELD(has_world, Field_EditAnywhere))
