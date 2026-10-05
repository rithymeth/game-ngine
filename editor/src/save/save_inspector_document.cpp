#include "save_inspector_document.h"

#include "aether/reflection/registry.h"

#include <new>

namespace aether::editor {

namespace stdfs = std::filesystem;

SaveInspectorDocument::~SaveInspectorDocument() { FreeInstance(); }

void SaveInspectorDocument::FreeInstance() {
    if (instance_ && type_) {
        if (type_->destruct) type_->destruct(instance_);
        ::operator delete(instance_, std::align_val_t(type_->alignment));
    }
    instance_ = nullptr;
}

void SaveInspectorDocument::Close() {
    FreeInstance();
    file_.clear();
    info_ = {};
    problems_.clear();
    type_ = nullptr;
    ++revision_;
}

bool SaveInspectorDocument::HasBackup() const {
    if (file_.empty()) return false;
    stdfs::path backup = file_;
    backup += ".bak";
    std::error_code ec;
    return stdfs::exists(backup, ec);
}

bool SaveInspectorDocument::Open(const stdfs::path& file) {
    FreeInstance();
    file_ = file;
    problems_.clear();
    type_ = nullptr;
    ++revision_;
    info_ = save::envelope::Inspect(file);
    if (!info_.ok) {
        problems_.push_back(info_.error);
        return false;
    }
    if (!info_.checksum_ok) problems_.push_back("the checksum doesn't match the data: the file was edited or damaged, and the game would refuse it");
    if (info_.format == 0 || info_.format > save::envelope::kFormat) problems_.push_back("an envelope format this version doesn't know");
    type_ = reflect::TypeRegistry::Find(info_.type);
    if (!type_) {
        problems_.push_back("'" + info_.type + "' isn't a type in this build, so the data is shown as JSON");
        return true;
    }
    if (info_.type_version > type_->version) {
        problems_.push_back("saved by a newer version of '" + info_.type + "' (" + std::to_string(info_.type_version) + ", this build has " + std::to_string(type_->version) + "): the game would refuse it");
    }
    if (type_->construct && type_->size > 0) {
        void* memory = ::operator new(type_->size, std::align_val_t(type_->alignment));
        type_->construct(memory);
        reflect::LoadReport report;
        if (reflect::FromJson(*type_, memory, info_.data, &report)) {
            instance_ = memory;
            for (const std::string& w : report.warnings) problems_.push_back("loading would skip: " + w);
        } else {
            if (type_->destruct) type_->destruct(memory);
            ::operator delete(memory, std::align_val_t(type_->alignment));
            problems_.push_back("the data doesn't have the shape of a '" + info_.type + "'");
            type_ = nullptr; // show it as JSON
        }
    }
    return true;
}

bool SaveInspectorDocument::OpenSlot(const save::SaveSystem& saves, const std::string& slot) {
    if (!save::SaveSystem::ValidSlotName(slot)) return false;
    return Open(saves.Directory() / (slot + ".asav"));
}

bool SaveInspectorDocument::DeleteFile() {
    if (file_.empty()) return false;
    std::error_code ec;
    stdfs::path backup = file_;
    backup += ".bak";
    const bool a = stdfs::remove(file_, ec);
    const bool b = stdfs::remove(backup, ec);
    if (!a && !b) return false;
    Close();
    return true;
}

bool SaveInspectorDocument::CopyTo(const std::string& name, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (file_.empty()) return fail("nothing is open");
    if (!save::SaveSystem::ValidSlotName(name)) return fail("'" + name + "' isn't a valid name (1-64 of A-Z a-z 0-9 _ -)");
    const stdfs::path target = file_.parent_path() / (name + file_.extension().string());
    std::error_code ec;
    if (stdfs::exists(target, ec)) return fail("'" + name + "' already exists");
    stdfs::copy_file(file_, target, ec);
    if (ec) return fail("couldn't copy: " + ec.message());
    stdfs::path backup = file_;
    backup += ".bak";
    if (stdfs::exists(backup, ec)) {
        stdfs::path target_backup = target;
        target_backup += ".bak";
        stdfs::copy_file(backup, target_backup, ec);
    }
    return true;
}

} // namespace aether::editor
