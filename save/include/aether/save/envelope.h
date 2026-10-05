#pragma once

#include "aether/reflection/reflection.h"
#include "aether/save/save_result.h"

#include <filesystem>
#include <string>
#include <string_view>

// The file format shared by save slots (kind "aether.save") and settings
// (kind "aether.settings"): a JSON envelope around a reflected struct's JSON,
// naming the kind, the struct's type and version, the time and a CRC-32 of
// the data, written atomically with the previous file kept as `<file>.bak`.
// SaveSystem and SettingsStore are the users; this is the mechanics.

namespace aether::save::envelope {

constexpr unsigned kFormat = 1;

// The file's contents for `object`. `name` is the slot or file name (informational).
std::string Make(std::string_view kind, std::string_view name, const reflect::TypeInfo& type, const void* object);

// Writes `contents` to `file` (making its folder), first copying an existing
// file to `<file>.bak`; the write itself is atomic.
SaveResult Write(const std::filesystem::path& file, std::string_view contents);

// Reads one file. NotFound, Corrupt (unreadable, a different kind of file,
// an unknown format, a checksum mismatch), WrongType, FutureVersion, or ok
// (with the load's warnings). The object is untouched on failure.
SaveResult Read(const std::filesystem::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object);

// Read, then on NotFound or Corrupt the `.bak` (with a note in the warnings).
SaveResult ReadWithBackup(const std::filesystem::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object);

SaveResult Fail(SaveError error, std::string message);
SaveResult Ok();

} // namespace aether::save::envelope
