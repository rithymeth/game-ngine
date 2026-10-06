#pragma once

#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"
#include "aether/save/save_result.h"

#include <filesystem>
#include <span>
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

// The same from bytes already in memory (`source` names them in messages). A damaged document, including one
// with a field of the wrong type, is Corrupt; nothing throws.
SaveResult ReadFromMemory(std::span<const u8> bytes, const std::string& source, std::string_view kind, const reflect::TypeInfo& type, void* object);

// Read, then on NotFound or Corrupt the `.bak` (with a note in the warnings).
SaveResult ReadWithBackup(const std::filesystem::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object);

// What a file claims to be, without loading it into a struct (for tools: the
// editor's Save Inspector). Never throws and never touches a game object.
struct EnvelopeInfo {
    bool ok = false;          // an envelope was read (even a damaged one: see checksum_ok)
    std::string error;        // why not, when !ok
    std::string kind;         // "aether.save", "aether.settings"
    u32 format = 0;
    std::string type;         // the saved struct's name
    u32 type_version = 0;
    std::string slot;
    u64 timestamp = 0;        // unix seconds
    std::string checksum_stored;
    bool checksum_ok = false; // the stored checksum matches the data, as the loader computes it
    u64 bytes = 0;            // the file's size
    std::string raw_text;     // the file as text
    reflect::Json data;       // the saved struct's JSON (null when !ok)
};
// Reads and checks a file's envelope. Files over 64 MB are refused.
EnvelopeInfo Inspect(const std::filesystem::path& file);
// The same on bytes already in memory.
EnvelopeInfo InspectBytes(std::span<const u8> bytes);

SaveResult Fail(SaveError error, std::string message);
SaveResult Ok();

} // namespace aether::save::envelope
