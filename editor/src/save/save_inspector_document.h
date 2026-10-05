#pragma once

#include "aether/reflection/reflection.h"
#include "aether/save/envelope.h"
#include "aether/save/save_system.h"

#include <filesystem>
#include <string>
#include <vector>

// A save or settings file open in the Save Inspector (Phase 28 step 5,
// docs/design/PHASE_SPECS.md §28.5): what the file says it is (kind, type,
// version, time, size), whether its checksum holds, whether a backup sits
// beside it, every problem found, and its contents, either reflected into a
// temporary instance of the saved struct when that type is registered in
// this build (so the generic Inspector can draw it) or as JSON.
//
// View-only on purpose: editing a save means recomputing its checksum,
// running migrations and silently changing a player's save. The actions are
// the ones that don't alter what a save contains: reload, delete (with its
// backup) and copy to a new slot.

namespace aether::editor {

class SaveInspectorDocument {
public:
    SaveInspectorDocument() = default;
    ~SaveInspectorDocument();
    SaveInspectorDocument(const SaveInspectorDocument&) = delete;
    SaveInspectorDocument& operator=(const SaveInspectorDocument&) = delete;

    // Reads the file's envelope; false (with Info().error) when it isn't a
    // save or settings file or can't be read. The file stays open (and its
    // error visible) either way, so a damaged file can be looked at.
    bool Open(const std::filesystem::path& file);
    bool OpenSlot(const save::SaveSystem& saves, const std::string& slot);
    bool Reload() { return Open(file_); }
    void Close();
    bool IsOpen() const { return !file_.empty(); }
    const std::filesystem::path& File() const { return file_; }
    u64 Revision() const { return revision_; }

    const save::envelope::EnvelopeInfo& Info() const { return info_; }
    bool HasBackup() const;
    // Everything wrong or worth knowing: a bad checksum, a type this build
    // doesn't have, a newer version than the code's, what loading would skip.
    const std::vector<std::string>& Problems() const { return problems_; }

    // The saved struct's type if this build has it registered, else null.
    const reflect::TypeInfo* Type() const { return type_; }
    // A temporary instance of Type() loaded from the file's data (null when
    // the type is unknown or the data doesn't fit it). Owned by the document.
    void* Instance() const { return instance_; }

    // Deletes the file and its backup and closes the document. False if it
    // couldn't (or nothing is open).
    bool DeleteFile();
    // Copies the file (and its backup) beside it as `<name>` plus the same
    // extension. `name` must be a valid slot name and not already taken.
    bool CopyTo(const std::string& name, std::string* error = nullptr);

private:
    void FreeInstance();

    std::filesystem::path file_;
    save::envelope::EnvelopeInfo info_;
    std::vector<std::string> problems_;
    const reflect::TypeInfo* type_ = nullptr;
    void* instance_ = nullptr;
    u64 revision_ = 1;
};

} // namespace aether::editor
