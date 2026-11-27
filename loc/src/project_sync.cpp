#include "aether/loc/project_sync.h"

#include "aether/loc/language.h"
#include "aether/loc/po.h"
#include "aether/platform/filesystem.h"

#include <algorithm>

namespace aether::loc {

namespace stdfs = std::filesystem;

namespace {

bool HasPoExtension(const stdfs::path& file) {
    const std::string ext = file.extension().string();
    return ext.size() == 3 && ext[0] == '.' && (ext[1] == 'p' || ext[1] == 'P') && (ext[2] == 'o' || ext[2] == 'O');
}

bool WriteText(const stdfs::path& file, const std::string& text) {
    const std::string path = file.string();
    // Compare raw bytes so CRLF and non-ASCII output are handled identically
    // on every host. A no-op sync should not wake file watchers.
    std::vector<u8> existing;
    if (fs::ReadFileBytes(path, existing, text.size()) && existing.size() == text.size() &&
        std::equal(text.begin(), text.end(), existing.begin(), [](char c, u8 byte) { return static_cast<u8>(c) == byte; })) {
        return true;
    }
    std::error_code ec;
    if (file.has_parent_path()) stdfs::create_directories(file.parent_path(), ec);
    return fs::WriteFileAtomic(path, text.data(), text.size());
}

} // namespace

SyncReport SyncProject(const SyncOptions& o) {
    SyncReport r;
    const std::string source_language = NormalizeLanguage(o.source_language);
    if (source_language.empty()) {
        r.messages.push_back("The source language is empty");
        return r;
    }
    std::error_code ec;
    if (!stdfs::is_directory(o.content_dir, ec)) {
        const std::string path = o.content_dir.empty() ? "(empty path)" : o.content_dir.string();
        r.messages.push_back("Couldn't open Content directory " + path + (ec ? ": " + ec.message() : ": not a directory"));
        return r;
    }
    r.gathered = GatherContent(o.content_dir);
    for (const std::string& w : r.gathered.warnings) r.messages.push_back(w);

    StringTable table;
    if (!o.strings_file.empty() && stdfs::exists(o.strings_file, ec)) {
        std::string csv;
        std::vector<std::string> errors;
        if (!fs::ReadFileText(o.strings_file.string(), csv) || !table.ImportCsv(csv, &errors)) {
            r.messages.push_back(o.strings_file.string() + " couldn't be read" + (errors.empty() ? "" : ": " + errors[0]));
            return r;
        }
        for (const std::string& e : errors) r.messages.push_back(o.strings_file.filename().string() + ": " + e);
    }

    if (!o.po_in.empty()) {
        std::vector<stdfs::path> files;
        const bool po_directory = stdfs::is_directory(o.po_in, ec);
        if (ec) {
            r.messages.push_back("Couldn't inspect PO input " + o.po_in.string() + ": " + ec.message());
            return r;
        }
        if (po_directory) {
            for (stdfs::directory_iterator it(o.po_in, ec), end; !ec && it != end; it.increment(ec)) {
                const auto& path = it->path();
                if (!HasPoExtension(path)) continue;
                const bool is_file = it->is_regular_file(ec);
                if (ec) {
                    r.messages.push_back("Couldn't inspect PO catalog " + path.string() + ": " + ec.message());
                    return r;
                }
                if (is_file) files.push_back(path);
            }
            // A failed directory open can produce the end iterator just like an empty directory.
            if (ec) {
                r.messages.push_back("Couldn't list PO directory " + o.po_in.string() + ": " + ec.message());
                return r;
            }
            std::sort(files.begin(), files.end());
        } else {
            files.push_back(o.po_in);
        }
        for (const stdfs::path& file : files) {
            std::string text;
            if (!fs::ReadFileText(file.string(), text)) {
                r.messages.push_back(file.string() + " couldn't be read");
                return r;
            }
            std::string language = PoLanguage(text);
            if (language.empty()) language = NormalizeLanguage(file.stem().string());
            if (language.empty() || language == source_language) {
                r.messages.push_back(file.filename().string() + ": no target language (its header has none and its name isn't one), so it was skipped");
                continue;
            }
            std::vector<std::string> errors;
            const bool good = ImportPo(table, language, text, &errors);
            for (const std::string& e : errors) r.messages.push_back(file.filename().string() + ": " + e);
            if (!good) return r;
            ++r.po_imported;
        }
    }

    std::vector<std::pair<std::string, std::string>> found;
    for (const GatheredText& t : r.gathered.entries) found.emplace_back(t.key, t.source);
    r.merge = MergeKeys(table, source_language, found);

    if (o.write && !o.strings_file.empty() && !WriteText(o.strings_file, table.ExportCsv())) {
        r.messages.push_back(o.strings_file.string() + " couldn't be written");
        return r;
    }
    if (!o.po_out.empty()) {
        std::vector<std::string> languages;
        for (const std::string& l : o.languages) languages.push_back(NormalizeLanguage(l));
        if (languages.empty()) {
            for (const std::string& l : table.Languages()) {
                if (l != source_language) languages.push_back(l);
            }
        }
        for (const std::string& language : languages) {
            if (language.empty() || language == source_language) continue;
            if (o.write && !WriteText(o.po_out / (language + ".po"), ExportPo(table, language, source_language))) {
                r.messages.push_back((o.po_out / (language + ".po")).string() + " couldn't be written");
                return r;
            }
            ++r.po_written;
        }
    }
    r.ok = true;
    return r;
}

} // namespace aether::loc
