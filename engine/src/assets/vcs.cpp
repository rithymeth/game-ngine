#include "aether/assets/vcs.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace aether::assets {

namespace stdfs = std::filesystem;

const char* VcsStateName(VcsState state) {
    switch (state) {
    case VcsState::Clean: return "Clean";
    case VcsState::Untracked: return "Untracked";
    case VcsState::Renamed: return "Renamed";
    case VcsState::Added: return "Added";
    case VcsState::Modified: return "Modified";
    case VcsState::Deleted: return "Deleted";
    case VcsState::Conflicted: return "Conflicted";
    }
    return "?";
}

char VcsStateLetter(VcsState state) {
    switch (state) {
    case VcsState::Clean: return ' ';
    case VcsState::Untracked: return '?';
    case VcsState::Renamed: return 'R';
    case VcsState::Added: return 'A';
    case VcsState::Modified: return 'M';
    case VcsState::Deleted: return 'D';
    case VcsState::Conflicted: return '!';
    }
    return ' ';
}

VcsState VcsStatus::Of(const std::string& repo_path) const {
    const auto it = files.find(repo_path);
    return it == files.end() ? VcsState::Clean : it->second;
}

VcsState VcsStatus::OfFolder(const std::string& repo_folder) const {
    std::string prefix = repo_folder;
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    VcsState worst = VcsState::Clean;
    for (const auto& [path, state] : files) {
        if (prefix.empty() || path.compare(0, prefix.size(), prefix) == 0) worst = WorseVcsState(worst, state);
    }
    return worst;
}

VcsState VcsStatus::OfLocalFolder(const std::string& local_folder) const {
    std::string folder = local_folder;
    if (!folder.empty() && folder.back() != '/') folder.push_back('/');
    return OfFolder(prefix + folder);
}

namespace {

VcsState FromCodes(char x, char y) {
    const auto either = [&](char c) { return x == c || y == c; };
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return VcsState::Conflicted;
    if (either('D')) return VcsState::Deleted;
    if (either('R') || either('C')) return VcsState::Renamed;
    if (either('A')) return VcsState::Added;
    if (either('M') || either('T')) return VcsState::Modified;
    return VcsState::Clean;
}

// Runs a command through the shell and captures its standard output.
bool Capture(const std::string& command, std::string& out, int* exit_code) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "rb");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return false;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) out.append(buffer, n);
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (exit_code) *exit_code = rc;
    return true;
}

} // namespace

VcsStatus ParseGitStatus(std::string_view output) {
    VcsStatus status;
    status.available = true;
    usize at = 0;
    while (at < output.size()) {
        usize end = output.find('\0', at);
        if (end == std::string_view::npos) end = output.size();
        const std::string_view entry = output.substr(at, end - at);
        at = end + 1;
        if (entry.size() < 4 || entry[2] != ' ') continue;
        const char x = entry[0], y = entry[1];
        std::string path(entry.substr(3));
        if (x == '!' && y == '!') continue; // ignored
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
            // The old name follows as its own NUL-terminated field.
            usize old_end = output.find('\0', at);
            if (old_end == std::string_view::npos) old_end = output.size();
            const std::string old_path(output.substr(at, old_end - at));
            at = old_end + 1;
            if (x == 'R' || y == 'R') status.files[old_path] = VcsState::Deleted; // the old name is gone
        }
        const VcsState state = (x == '?' && y == '?') ? VcsState::Untracked : FromCodes(x, y);
        if (state != VcsState::Clean) status.files[path] = state;
    }
    return status;
}

VcsStatus QueryGitStatus(const stdfs::path& directory) {
    VcsStatus status;
    const std::string dir = directory.string();
    if (dir.find('"') != std::string::npos) {
        status.error = "The path has a quote in it";
        return status;
    }
#if defined(_WIN32)
    const char* quiet = " 2>nul";
#else
    const char* quiet = " 2>/dev/null";
#endif
    std::string prefix_out;
    int rc = 0;
    if (!Capture("git -C \"" + dir + "\" rev-parse --show-prefix" + quiet, prefix_out, &rc) || rc != 0) {
        status.error = "Not in a git repository (or git isn't installed)";
        return status;
    }
    while (!prefix_out.empty() && (prefix_out.back() == '\n' || prefix_out.back() == '\r')) prefix_out.pop_back();
    std::string output;
    if (!Capture("git -C \"" + dir + "\" status --porcelain=v1 -z --untracked-files=all -- ." + quiet, output, &rc) || rc != 0) {
        status.error = "git status failed";
        return status;
    }
    status = ParseGitStatus(output);
    status.prefix = prefix_out;
    return status;
}

} // namespace aether::assets
