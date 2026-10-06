#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"
#include "aether/save/save_system.h"

#include <string>

// Blueprint function library for saving (Phase 28 step 4, §28.4): Save Game to
// Slot, Load Game from Slot, Does Save Game Exist, Create Save Game Object,
// and typed Set / Get nodes over the active SaveSystem's bag of named values
// (SaveBag), plus Capture / Restore World for the entities marked
// SaveableEntity. Every function acts on SaveSystem::Active() and does
// nothing (false, a default, an empty string) without one; a function that
// can fail returns false and leaves the reason in GetLastError. The same
// functions are Luau's `SaveGames` table (scripting/save_api.h).
//
// Saving a bag is saving the struct "SaveBag" to the slot, so C++ code can
// Load("slot", bag) what a Blueprint saved, and the reverse.

namespace aether::save {

struct SaveGames {
    // A fresh, empty save object: clears the bag (and its world snapshot).
    static void CreateSaveObject();

    static void SetBool(const std::string& key, bool value);
    static void SetInt(const std::string& key, i32 value);
    static void SetFloat(const std::string& key, f32 value);
    static void SetString(const std::string& key, const std::string& value);
    static void SetVector(const std::string& key, const Vec3& value);
    static bool GetBool(const std::string& key, bool fallback);
    static i32 GetInt(const std::string& key, i32 fallback);
    static f32 GetFloat(const std::string& key, f32 fallback);
    static std::string GetString(const std::string& key, const std::string& fallback);
    static Vec3 GetVector(const std::string& key, const Vec3& fallback);
    static bool HasKey(const std::string& key);
    static bool RemoveKey(const std::string& key);

    // Writes the bag to the slot; false (see GetLastError) on an invalid slot name or a write failure.
    static bool SaveToSlot(const std::string& slot);
    // Writes it on a worker thread: the bag is copied now; when the host pumps
    // the system, Event.OnSaveFinished (slot, success) is dispatched.
    static void SaveToSlotAsync(const std::string& slot);
    // Replaces the bag with the slot's (unchanged on failure).
    static bool LoadFromSlot(const std::string& slot);
    static bool DoesSaveExist(const std::string& slot);
    static bool DeleteSave(const std::string& slot);
    static i32 GetSlotCount();
    static std::string GetSlotName(i32 index); // by name order; "" out of range
    static std::string GetLastError();

    // Captures every SaveableEntity into the bag / puts the bag's snapshot back
    // into the world the host gave the system (SetWorldContext). Restore
    // returns false when something couldn't be restored (the rest was); the
    // reasons are in GetLastError.
    static bool CaptureWorld();
    static bool RestoreWorld();
};

} // namespace aether::save

AETHER_REFLECT(aether::save::SaveGames, 1,
    AETHER_METHOD(CreateSaveObject, Fn_BlueprintCallable),
    AETHER_METHOD(SetBool, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetInt, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetFloat, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetString, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetVector, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(GetBool, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(GetInt, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(GetFloat, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(GetString, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(GetVector, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(HasKey, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(RemoveKey, Fn_BlueprintCallable, {"key"}),
    AETHER_METHOD(SaveToSlot, Fn_BlueprintCallable, {"slot"}),
    AETHER_METHOD(SaveToSlotAsync, Fn_BlueprintCallable, {"slot"}),
    AETHER_METHOD(LoadFromSlot, Fn_BlueprintCallable, {"slot"}),
    AETHER_METHOD(DoesSaveExist, Fn_BlueprintCallable | Fn_Pure, {"slot"}),
    AETHER_METHOD(DeleteSave, Fn_BlueprintCallable, {"slot"}),
    AETHER_METHOD(GetSlotCount, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetSlotName, Fn_BlueprintCallable | Fn_Pure, {"index"}),
    AETHER_METHOD(GetLastError, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(CaptureWorld, Fn_BlueprintCallable),
    AETHER_METHOD(RestoreWorld, Fn_BlueprintCallable)
)
