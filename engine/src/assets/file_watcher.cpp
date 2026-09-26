#include "aether/assets/file_watcher.h"

#include <system_error>

namespace aether::assets {

namespace stdfs = std::filesystem;

FileWatcher::FileWatcher(stdfs::path root, std::chrono::milliseconds debounce)
    : root_(std::move(root)), debounce_(debounce) {
    files_ = Snapshot();
}

std::map<std::string, FileWatcher::FileState> FileWatcher::Snapshot() const {
    std::map<std::string, FileState> files;
    std::error_code ec;
    if (!stdfs::is_directory(root_, ec)) {
        return files;
    }
    for (auto it = stdfs::recursive_directory_iterator(root_, ec); !ec && it != stdfs::recursive_directory_iterator();
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec)) {
            continue;
        }
        // A file can vanish between listing and reading its details.
        FileState state;
        state.written = it->last_write_time(file_ec);
        if (file_ec) {
            continue;
        }
        state.size = it->file_size(file_ec);
        if (file_ec) {
            continue;
        }
        files[stdfs::relative(it->path(), root_, file_ec).generic_string()] = state;
    }
    return files;
}

void FileWatcher::Note(const std::string& path, FileChange::Kind kind, Clock::time_point now) {
    using Kind = FileChange::Kind;
    auto it = pending_.find(path);
    if (it == pending_.end()) {
        pending_[path] = {kind, now};
        return;
    }
    // Combine with the change already waiting for this file.
    Kind combined = kind;
    const Kind earlier = it->second.kind;
    if (earlier == Kind::Added && kind == Kind::Removed) {
        pending_.erase(it); // came and went: nothing to report
        return;
    }
    if (earlier == Kind::Added) {
        combined = Kind::Added; // still new
    } else if (earlier == Kind::Removed && kind == Kind::Added) {
        combined = Kind::Modified; // replaced (write-temp-then-rename)
    }
    it->second = {combined, now};
}

void FileWatcher::Refresh(const std::string& path) {
    pending_.erase(path);
    std::error_code ec;
    const stdfs::path file = root_ / path;
    FileState state;
    state.written = stdfs::last_write_time(file, ec);
    if (!ec) {
        state.size = stdfs::file_size(file, ec);
    }
    if (ec) {
        files_.erase(path);
    } else {
        files_[path] = state;
    }
}

std::vector<FileChange> FileWatcher::Poll(Clock::time_point now) {
    std::map<std::string, FileState> current = Snapshot();
    for (const auto& [path, state] : current) {
        auto old = files_.find(path);
        if (old == files_.end()) {
            Note(path, FileChange::Kind::Added, now);
        } else if (!(old->second == state)) {
            Note(path, FileChange::Kind::Modified, now);
        }
    }
    for (const auto& [path, state] : files_) {
        if (current.count(path) == 0) {
            Note(path, FileChange::Kind::Removed, now);
        }
    }
    files_ = std::move(current);

    std::vector<FileChange> settled;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.last_seen >= debounce_) {
            settled.push_back({it->second.kind, it->first});
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return settled; // std::map order: sorted by path
}

} // namespace aether::assets
