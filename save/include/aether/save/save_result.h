#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::save {

enum class SaveError : u8 {
    None,
    InvalidSlot,   // a slot name must be 1-64 of A-Z a-z 0-9 _ -
    IoError,       // couldn't read or write the file
    Corrupt,       // not a save file, truncated, or its checksum doesn't match
    WrongType,     // the slot holds a different struct
    FutureVersion, // saved by a newer version of the struct than this code has
    NotFound,
};

// Ignoring a save or load result loses the failure, so the compiler warns (Phase 37 step 7).
struct [[nodiscard]] SaveResult {
    bool ok = false;
    SaveError error = SaveError::None;
    std::string message;
    std::vector<std::string> warnings; // what loading skipped or changed (and a note when the backup was used)
};

} // namespace aether::save
