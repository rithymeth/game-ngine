#pragma once

#include "aether/core/base.h"
#include "aether/pak/pak.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::pak {

// The virtual file system the cooked game reads from (Phase 25 step 1):
// directories and .apak archives mounted at mount points, highest priority
// first. A path is read from the first mount that has it, so a patch or
// DLC pak mounted above the base pak overrides its files, and a loose
// directory above both overrides everything (for iterating on content).
class VirtualFileSystem {
public:
    // Mounts a directory or an archive (by its .apak extension) at
    // `mount_point` ("" for the root). Higher priority wins; equal
    // priorities go by mount order, later first. False when it can't be
    // opened.
    bool Mount(const std::string& source, const std::string& mount_point = "", int priority = 0,
               std::string* error = nullptr);
    // Mounts an already-open archive.
    void MountPak(PakReader reader, const std::string& mount_point = "", int priority = 0);
    bool Unmount(const std::string& source);
    void UnmountAll() { mounts_.clear(); }
    usize MountCount() const { return mounts_.size(); }

    bool Exists(const std::string& path) const;
    bool Read(const std::string& path, std::vector<u8>& out, std::string* error = nullptr) const;
    bool ReadText(const std::string& path, std::string& out, std::string* error = nullptr) const;
    // Which mount a path resolves to (its source), or "".
    std::string Resolve(const std::string& path) const;
    // Every visible path starting with `prefix`, each once, sorted.
    std::vector<std::string> List(const std::string& prefix = "") const;

private:
    struct MountEntry {
        std::string source;
        std::string mount_point; // normalized, "" or ending in '/'
        int priority = 0;
        u64 order = 0;
        bool is_pak = false;
        std::shared_ptr<PakReader> pak;
    };
    // The path inside the mount, or false when the path isn't under it.
    static bool Inner(const MountEntry& m, const std::string& path, std::string& inner);
    void Sort();

    std::vector<MountEntry> mounts_;
    u64 next_order_ = 0;
};

} // namespace aether::pak
