#include "localization_document.h"

#include "aether/loc/language.h"
#include "aether/loc/po.h"
#include "aether/platform/filesystem.h"

namespace aether::editor {

namespace stdfs = std::filesystem;

bool LocalizationDocument::Open(const stdfs::path& file) {
    file_ = file;
    table_.Clear();
    errors_.clear();
    dirty_ = false;
    ++revision_;
    std::string text;
    if (!fs::ReadFileText(file.string(), text)) {
        errors_.push_back("Couldn't read " + file.string());
        return false;
    }
    const bool ok = table_.ImportCsv(text, &errors_);
    const std::vector<std::string> languages = table_.Languages();
    const std::string en = loc::NormalizeLanguage(source_language_);
    bool has_source = false;
    for (const std::string& l : languages) has_source = has_source || l == en;
    if (!has_source && !languages.empty()) source_language_ = languages.front();
    return ok;
}

void LocalizationDocument::Close() {
    file_.clear();
    table_.Clear();
    errors_.clear();
    dirty_ = false;
    last_gather_ = {};
    ++revision_;
}

bool LocalizationDocument::Save() {
    if (!IsOpen()) return false;
    const std::string csv = table_.ExportCsv();
    std::error_code ec;
    if (file_.has_parent_path()) stdfs::create_directories(file_.parent_path(), ec);
    if (!fs::WriteFileAtomic(file_.string(), csv.data(), csv.size())) {
        errors_.push_back("Couldn't write " + file_.string());
        return false;
    }
    dirty_ = false;
    return true;
}

void LocalizationDocument::SetSourceLanguage(const std::string& language) {
    const std::string l = loc::NormalizeLanguage(language);
    if (l.empty() || l == source_language_) return;
    source_language_ = l;
    ++revision_;
}

std::vector<LocalizationDocument::LangStat> LocalizationDocument::Stats() const {
    std::vector<LangStat> out;
    const usize total = table_.KeyCount();
    for (const std::string& l : table_.Languages()) {
        LangStat s;
        s.language = l;
        s.count = table_.Count(l);
        s.total = total;
        s.percent = total == 0 ? 100.0f : 100.0f * static_cast<f32>(s.count) / static_cast<f32>(total);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::string> LocalizationDocument::MissingKeys(const std::string& language) const {
    std::vector<std::string> out;
    for (const std::string& key : table_.Keys()) {
        const std::string* text = table_.Find(language, key);
        if (text == nullptr || text->empty()) out.push_back(key);
    }
    return out;
}

bool LocalizationDocument::SetCell(const std::string& language, const std::string& key, const std::string& text) {
    if (loc::NormalizeLanguage(language).empty() || !table_.HasKey(key)) return false;
    const std::string* current = table_.Find(language, key);
    if (text.empty()) {
        if (current == nullptr) return true;
        table_.Remove(language, key);
        table_.AddLanguage(language); // a column stays even when its last cell is cleared
    } else {
        if (current != nullptr && *current == text) return true;
        table_.Set(language, key, text);
    }
    Touch();
    return true;
}

bool LocalizationDocument::AddKey(const std::string& key, const std::string& source_text) {
    if (key.empty() || table_.HasKey(key)) return false;
    table_.Set(source_language_, key, source_text.empty() ? key : source_text);
    Touch();
    return true;
}

bool LocalizationDocument::RemoveKey(const std::string& key) {
    if (!table_.HasKey(key)) return false;
    const std::vector<std::string> languages = table_.Languages();
    for (const std::string& l : languages) {
        table_.Remove(l, key);
        table_.AddLanguage(l);
    }
    Touch();
    return true;
}

bool LocalizationDocument::AddLanguage(const std::string& language) {
    const std::string l = loc::NormalizeLanguage(language);
    if (l.empty()) return false;
    for (const std::string& existing : table_.Languages()) {
        if (existing == l) return false;
    }
    table_.AddLanguage(l);
    Touch();
    return true;
}

bool LocalizationDocument::ImportPo(const std::string& language, const std::string& po_text) {
    std::string target = loc::PoLanguage(po_text);
    if (target.empty()) target = loc::NormalizeLanguage(language);
    if (target.empty() || target == source_language_) {
        errors_.push_back("The .po file's language isn't a translation language");
        return false;
    }
    std::vector<std::string> errors;
    const bool ok = loc::ImportPo(table_, target, po_text, &errors);
    for (std::string& e : errors) errors_.push_back("po: " + e);
    Touch();
    return ok;
}

bool LocalizationDocument::ExportPo(const std::string& language, const stdfs::path& file) const {
    const std::string po = loc::ExportPo(table_, language, source_language_);
    std::error_code ec;
    if (file.has_parent_path()) stdfs::create_directories(file.parent_path(), ec);
    return fs::WriteFileAtomic(file.string(), po.data(), po.size());
}

bool LocalizationDocument::Gather(const stdfs::path& content_dir, bool check) {
    if (!IsOpen()) return false;
    if (!check && dirty_) {
        errors_.push_back("Save the table before gathering: the gather rewrites the file");
        return false;
    }
    loc::SyncOptions o;
    o.content_dir = content_dir;
    o.strings_file = file_;
    o.source_language = source_language_;
    o.write = !check;
    last_gather_ = loc::SyncProject(o);
    if (last_gather_.ok && !check) Open(file_);
    return last_gather_.ok;
}

} // namespace aether::editor
