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

EnvelopeInfo Inspect(const stdfs::path& file) {
    EnvelopeInfo info;
    std::error_code ec;
    const auto size = stdfs::file_size(file, ec);
    if (ec) {
        info.error = "can't read " + file.string();
        return info;
    }
    info.bytes = size;
    if (size > 64ull * 1024 * 1024) {
        info.error = "the file is too large to inspect (over 64 MB)";
        return info;
    }
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        info.error = "can't read " + file.string();
        return info;
    }
    EnvelopeInfo parsed = InspectBytes(bytes);
    parsed.bytes = info.bytes;
    return parsed;
}

EnvelopeInfo InspectBytes(std::span<const u8> bytes) {
    EnvelopeInfo info;
    info.bytes = bytes.size();
    info.raw_text.assign(bytes.begin(), bytes.end());
    const Json j = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("data") || !j.contains("$type")) {
        info.error = "not an Aether save or settings file";
        return info;
    }
    // A field of the wrong type (a number where the name goes) makes nlohmann's value() throw; that is a damaged file.
    try {
        info.kind = j.value("$type", "");
        info.format = j.value("format", 0u);
        info.type = j.value("type", "");
        info.type_version = j.value("type_version", 0u);
        info.slot = j.value("slot", "");
        info.timestamp = j.value("timestamp_utc", static_cast<u64>(0));
        info.checksum_stored = j.value("checksum", "");
    } catch (const Json::exception& e) {
        info = EnvelopeInfo();
        info.bytes = bytes.size();
        info.raw_text.assign(bytes.begin(), bytes.end());
        info.error = std::string("the envelope has a field of the wrong type: ") + e.what();
        return info;
    }
    info.ok = true;
    info.data = j["data"];
    info.checksum_ok = info.checksum_stored == Hex(Checksum(info.data.dump()));
    return info;
}

SaveResult Read(const stdfs::path& file, std::string_view kind, const reflect::TypeInfo& type, void* object) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return Fail(SaveError::NotFound, "no file at " + file.string());
    return ReadFromMemory(bytes, file.string(), kind, type, object);
}

SaveResult ReadFromMemory(std::span<const u8> bytes, const std::string& source, std::string_view kind, const reflect::TypeInfo& type, void* object) {
    const Json envelope = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (envelope.is_discarded() || !envelope.is_object() || !envelope.contains("data")) {
        return Fail(SaveError::Corrupt, source + " isn't a readable " + std::string(kind) + " file");
    }
    // A field of the wrong type makes nlohmann's value() throw: that is a damaged file, not a crash.
    try {
        if (envelope.value("$type", "") != kind) {
            return Fail(SaveError::Corrupt, source + " isn't a readable " + std::string(kind) + " file");
        }
        if (envelope.value("format", 0u) == 0 || envelope.value("format", 0u) > kFormat) {
            return Fail(SaveError::Corrupt, source + " has a file format this version doesn't know");
        }
        if (envelope.value("checksum", "") != Hex(Checksum(envelope["data"].dump()))) {
            return Fail(SaveError::Corrupt, source + " has been changed or damaged (its checksum doesn't match)");
        }
        if (envelope.value("type", "") != type.name) {
            return Fail(SaveError::WrongType, source + " holds a '" + envelope.value("type", "") + "', not a '" + type.name + "'");
        }
        if (envelope.value("type_version", 0u) > type.version) {
            return Fail(SaveError::FutureVersion, source + " was saved by a newer version of '" + type.name + "' (" +
                                                      std::to_string(envelope.value("type_version", 0u)) + ", this code has " + std::to_string(type.version) + ")");
        }
    } catch (const Json::exception&) {
        return Fail(SaveError::Corrupt, source + " has a field of the wrong type");
    }
    reflect::LoadReport report;
    if (!reflect::FromJson(type, object, envelope["data"], &report)) {
        return Fail(SaveError::Corrupt, source + " doesn't have the shape of a '" + type.name + "'");
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
