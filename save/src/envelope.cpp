#include "aether/save/envelope.h"

#include "aether/core/log.h"
#include "aether/pak/pak.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <cstdio>
#include <ctime>

namespace aether::save::envelope {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

u32 Checksum(const std::string& text) {
    return pak::Crc32(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

std::string Hex(u32 value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", value);
    return buf;
}

} // namespace

SaveResult Fail(SaveError error, std::string message) {
    SaveResult r;
    r.error = error;
    r.message = std::move(message);
    AETHER_LOG_WARN("Save", "%s", r.message.c_str());
    return r;
}

SaveResult Ok() {
    SaveResult r;
    r.ok = true;
    return r;
}

std::string Make(std::string_view kind, std::string_view name, const reflect::TypeInfo& type, const void* object) {
    Json data = reflect::ToJson(type, object);
    const std::string dumped = data.dump();
    Json envelope = {{"$type", std::string(kind)},
                     {"format", kFormat},
                     {"type", type.name},
                     {"type_version", type.version},
                     {"slot", std::string(name)},
                     {"timestamp_utc", static_cast<u64>(std::time(nullptr))},
                     {"checksum", Hex(Checksum(dumped))},
                     {"data", std::move(data)}};
    return envelope.dump(2);
}

SaveResult Write(const stdfs::path& file, std::string_view contents) {
    std::error_code ec;
    if (!file.parent_path().empty()) stdfs::create_directories(file.parent_path(), ec);
    if (stdfs::exists(file, ec)) {
        // Keep the previous file: if this write or the new file turns out bad, it is still there.
        stdfs::path backup = file;
        backup += ".bak";
        stdfs::copy_file(file, backup, stdfs::copy_options::overwrite_existing, ec);
    }
    if (!fs::WriteFileAtomic(file.string(), contents.data(), contents.size())) {
        return Fail(SaveError::IoError, "couldn't write " + file.string());
    }
    return Ok();
}

SaveResult Read(const stdfs::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return Fail(SaveError::NotFound, "no file at " + file.string());
    const Json envelope = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (envelope.is_discarded() || !envelope.is_object() || envelope.value("$type", "") != kind || !envelope.contains("data")) {
        return Fail(SaveError::Corrupt, file.string() + " isn't a readable " + std::string(kind) + " file");
    }
    if (envelope.value("format", 0u) == 0 || envelope.value("format", 0u) > kFormat) {
        return Fail(SaveError::Corrupt, file.string() + " has a file format this version doesn't know");
    }
    if (envelope.value("checksum", "") != Hex(Checksum(envelope["data"].dump()))) {
        return Fail(SaveError::Corrupt, file.string() + " has been changed or damaged (its checksum doesn't match)");
    }
    if (envelope.value("type", "") != type.name) {
        return Fail(SaveError::WrongType, file.string() + " holds a '" + envelope.value("type", "") + "', not a '" + type.name + "'");
    }
    if (envelope.value("type_version", 0u) > type.version) {
        return Fail(SaveError::FutureVersion, file.string() + " was saved by a newer version of '" + type.name + "' (" +
                                                  std::to_string(envelope.value("type_version", 0u)) + ", this code has " + std::to_string(type.version) + ")");
    }
    reflect::LoadReport report;
    if (!reflect::FromJson(type, object, envelope["data"], &report)) {
        return Fail(SaveError::Corrupt, file.string() + " doesn't have the shape of a '" + type.name + "'");
    }
    SaveResult r = Ok();
    r.warnings = std::move(report.warnings);
    return r;
}

SaveResult ReadWithBackup(const stdfs::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object) {
    stdfs::path backup = file;
    backup += ".bak";
    SaveResult result = Read(file, kind, type, object);
    if (result.ok) return result;
    // A wrong type or a newer version is the caller's to hear about; damage or a missing file falls back to the backup.
    if ((result.error == SaveError::Corrupt || result.error == SaveError::NotFound) && fs::Exists(backup.string())) {
        SaveResult from_backup = Read(backup, kind, type, object);
        if (from_backup.ok) {
            from_backup.warnings.push_back("the file was unreadable (" + result.message + "): loaded the previous one");
            return from_backup;
        }
    }
    return result;
}

} // namespace aether::save::envelope
