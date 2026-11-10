#include "aether/assets/file_watcher.h"

#include <system_error>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aether::assets {

namespace stdfs = std::filesystem;

// ---- Native notifications ---------------------------------------------------------------------------------------------------
// A Native only answers "did anything happen since I was last asked?". It never reports what, so a missed or coalesced event cannot
// make the watcher wrong: the walk that follows compares real file states.
#if defined(__linux__)

struct FileWatcher::Native {
    int fd = -1;
    std::map<int, std::string> dirs; // watch descriptor -> directory
    bool degraded = false;           // a watch could not be added (limit reached): every Poll walks

    static constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                           IN_DELETE_SELF | IN_MOVE_SELF;

    ~Native() {
        if (fd >= 0) close(fd);
    }

    bool Start(const stdfs::path& root) {
        std::error_code ec;
        if (!stdfs::is_directory(root, ec)) return false;
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0) return false;
        AddTree(root);
        return !degraded;
    }

    void AddWatch(const stdfs::path& dir) {
        const int wd = inotify_add_watch(fd, dir.c_str(), kMask);
        if (wd < 0) degraded = true;
        else dirs[wd] = dir.string();
    }

    void AddTree(const stdfs::path& root) {
        AddWatch(root);
        std::error_code ec;
        for (auto it = stdfs::recursive_directory_iterator(root, ec); !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!name.empty() && name.front() == '.') {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            std::error_code dir_ec;
            if (it->is_directory(dir_ec) && !it->is_symlink(dir_ec)) AddWatch(it->path());
        }
    }

    bool TakeChanged() {
        if (degraded) return true;
        bool changed = false;
        alignas(inotify_event) char buffer[16384];
        for (;;) {
            const ssize_t length = read(fd, buffer, sizeof(buffer));
            if (length <= 0) break; // EAGAIN: drained
            for (char* at = buffer; at < buffer + length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(at);
                at += sizeof(inotify_event) + event->len;
                if (event->mask & IN_Q_OVERFLOW) {
                    changed = true;
                    continue;
                }
                if (event->mask & IN_IGNORED) { // the watched folder went away
                    dirs.erase(event->wd);
                    changed = true;
                    continue;
                }
                const bool hidden = event->len > 0 && event->name[0] == '.';
                if (hidden) continue; // same rule as the walk: dot files and folders are not watched content
                if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                    const auto parent = dirs.find(event->wd);
                    if (parent != dirs.end()) AddTree(stdfs::path(parent->second) / event->name); // the walk finds what is already inside
                }
                changed = true;
            }
        }
        return changed;
    }
};

#elif defined(_WIN32)

struct FileWatcher::Native {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    alignas(DWORD) char buffer[64 * 1024];
    bool armed = false;

    ~Native() {
        if (directory != INVALID_HANDLE_VALUE) {
            if (armed) {
                CancelIoEx(directory, &overlapped);
                DWORD bytes = 0;
                GetOverlappedResult(directory, &overlapped, &bytes, TRUE); // the kernel must be done with `buffer` before it goes
            }
            CloseHandle(directory);
        }
        if (event) CloseHandle(event);
    }

    bool Start(const stdfs::path& root) {
        directory = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) return false;
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return event != nullptr && Arm();
    }

    bool Arm() {
        ResetEvent(event);
        overlapped = OVERLAPPED{};
        overlapped.hEvent = event;
        DWORD ignored = 0;
        armed = ReadDirectoryChangesW(directory, buffer, sizeof(buffer), TRUE,
                                      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                                      &ignored, &overlapped, nullptr) != 0;
        return armed;
    }

    bool TakeChanged() {
        if (!armed) return true; // could not be re-armed: fall back to walking every time
        DWORD bytes = 0;
        if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return false;
        }
        Arm(); // a completed read (zero bytes means the buffer overflowed) or an error: either way something may have changed
        return true;
    }
};

#else

struct FileWatcher::Native {
    bool Start(const stdfs::path&) { return false; } // no native backend on this platform yet (macOS: FSEvents): polls
    bool TakeChanged() { return true; }
};

#endif

FileWatcher::FileWatcher(stdfs::path root, std::chrono::milliseconds debounce, Mode mode)
    : root_(std::move(root)), debounce_(debounce) {
    if (mode == Mode::Native) {
        auto native = std::make_unique<Native>();
        if (native->Start(root_)) native_ = std::move(native);
    }
    files_ = Snapshot(); // after the watches exist, so nothing changing in between is missed
}

FileWatcher::~FileWatcher() = default;
FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

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
    // With native notifications the walk happens only when the OS said something changed, or the safety rescan is due. The
    // notification is always drained, so the next Poll is not told about this one's events again.
    bool walk = true;
    if (native_) {
        const bool signalled = native_->TakeChanged();
        walk = signalled || now - last_scan_ >= kSafetyRescan;
    }
    if (!walk) return SettledBy(now);
    last_scan_ = now;
    ++scans_;
    std::map<std::string, FileState> current = Snapshot();
    auto old = files_.begin();
    auto fresh = current.begin();
    while (old != files_.end() && fresh != current.end()) {
        if (old->first < fresh->first) {
            Note(old->first, FileChange::Kind::Removed, now);
            ++old;
        } else if (fresh->first < old->first) {
            Note(fresh->first, FileChange::Kind::Added, now);
            ++fresh;
        } else {
            if (!(old->second == fresh->second)) {
                Note(fresh->first, FileChange::Kind::Modified, now);
            }
            ++old;
            ++fresh;
        }
    }
    for (; old != files_.end(); ++old) {
        Note(old->first, FileChange::Kind::Removed, now);
    }
    for (; fresh != current.end(); ++fresh) {
        Note(fresh->first, FileChange::Kind::Added, now);
    }
    files_ = std::move(current);
    return SettledBy(now);
}

std::vector<FileChange> FileWatcher::SettledBy(Clock::time_point now) {
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
