#include "core/file_dialog.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros would break std::min below
#endif
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace aether::editor {
namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string Narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr, nullptr);
    narrow.pop_back(); // the terminator the conversion wrote
    return narrow;
}

// "ascene;aesc" -> L"*.ascene;*.aesc"; "*" -> L"*.*".
std::wstring Patterns(const std::string& extensions) {
    std::wstring patterns;
    std::size_t start = 0;
    while (start <= extensions.size()) {
        const std::size_t end = std::min(extensions.find(';', start), extensions.size());
        const std::string extension = extensions.substr(start, end - start);
        if (!patterns.empty()) patterns += L';';
        patterns += extension == "*" ? L"*.*" : Widen("*." + extension);
        if (end == extensions.size()) break;
        start = end + 1;
    }
    return patterns;
}

class WindowsFileDialog final : public FileDialog {
public:
    bool Available() const override { return true; }

    std::filesystem::path Show(const FileDialogOptions& options) override {
        // The dialog needs an apartment-threaded caller. If the host already set one up differently, Show still runs and
        // fails cleanly rather than tearing down the host's COM.
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool uninitialize = SUCCEEDED(initialized);

        IFileDialog* dialog = nullptr;
        HRESULT hr = CoCreateInstance(options.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || dialog == nullptr) {
            if (uninitialize) CoUninitialize();
            return {};
        }

        DWORD flags = 0;
        dialog->GetOptions(&flags);
        flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        flags |= options.save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
        dialog->SetOptions(flags);
        dialog->SetTitle(Widen(options.title).c_str());
        if (!options.default_extension.empty()) dialog->SetDefaultExtension(Widen(options.default_extension).c_str());
        if (!options.filters.empty()) {
            std::vector<std::wstring> labels;
            std::vector<std::wstring> patterns;
            std::vector<COMDLG_FILTERSPEC> specs;
            labels.reserve(options.filters.size());
            patterns.reserve(options.filters.size());
            specs.reserve(options.filters.size());
            for (const FileDialogFilter& filter : options.filters) {
                labels.push_back(Widen(filter.label));
                patterns.push_back(Patterns(filter.extensions));
            }
            for (std::size_t i = 0; i < labels.size(); ++i) specs.push_back({labels[i].c_str(), patterns[i].c_str()});
            dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        }

        std::error_code ec;
        if (!options.start_path.empty()) {
            const std::filesystem::path folder =
                std::filesystem::is_directory(options.start_path, ec) ? options.start_path : options.start_path.parent_path();
            if (!folder.empty()) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))) && item != nullptr) {
                    dialog->SetFolder(item);
                    item->Release();
                }
            }
        }
        if (options.save) {
            const std::string name = options.default_name.empty() ? options.start_path.filename().generic_string()
                                                                  : options.default_name;
            if (!name.empty()) dialog->SetFileName(Widen(name).c_str());
        }

        hr = dialog->Show(nullptr);
        std::filesystem::path chosen;
        if (SUCCEEDED(hr)) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)) && result != nullptr) {
                PWSTR file_path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &file_path))) {
                    chosen = std::filesystem::path(Narrow(file_path));
                    CoTaskMemFree(file_path);
                }
                result->Release();
            }
        }
        dialog->Release();
        if (uninitialize) CoUninitialize();
        return chosen;
    }
};

} // namespace

std::unique_ptr<FileDialog> CreateNativeFileDialog() { return std::make_unique<WindowsFileDialog>(); }

} // namespace aether::editor

#elif defined(__linux__) || defined(__APPLE__)

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {
namespace {

struct ProcessResult {
    bool ran = false; // the program exists and started
    bool ok = false;  // it exited 0 and printed a path
    std::string output;
};

// Runs argv with argv[0] resolved on PATH, without a shell, and collects stdout. Exit status 127 is how execvp reports
// "no such program", which is what tells an unavailable picker apart from a cancelled one.
ProcessResult Run(const std::vector<std::string>& arguments) {
    ProcessResult result;
    int fds[2];
    if (pipe(fds) != 0) return result;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buffer[512];
    for (ssize_t read_bytes; (read_bytes = read(fds[0], buffer, sizeof(buffer))) > 0;) result.output.append(buffer, static_cast<size_t>(read_bytes));
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return result;
    if (!WIFEXITED(status)) return result;
    result.ran = WEXITSTATUS(status) != 127;
    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) result.output.pop_back();
    return result;
}

std::vector<std::string> Patterns(const std::string& extensions) {
    std::vector<std::string> patterns;
    std::size_t start = 0;
    while (start <= extensions.size()) {
        const std::size_t end = std::min(extensions.find(';', start), extensions.size());
        const std::string extension = extensions.substr(start, end - start);
        patterns.push_back(extension == "*" ? "*" : "*." + extension);
        if (end == extensions.size()) break;
        start = end + 1;
    }
    return patterns;
}

std::string Joined(std::vector<std::string> patterns, char separator) {
    std::string joined;
    for (const std::string& pattern : patterns) {
        if (!joined.empty()) joined += separator;
        joined += pattern;
    }
    return joined;
}

class ZenityFileDialog final : public FileDialog {
public:
    bool Available() const override {
        if (!found_.has_value()) found_ = FindAvailable();
        return *found_;
    }

    std::filesystem::path Show(const FileDialogOptions& options) override {
        chosen_.clear();
        std::filesystem::path start = options.start_path;
        if (options.save && !options.default_name.empty()) {
            std::error_code ec;
            const std::filesystem::path folder = std::filesystem::is_directory(start, ec) ? start : start.parent_path();
            start = folder / options.default_name;
        }
        if (TryZenity(options, start)) return chosen_;
        if (TryKdialog(options, start)) return chosen_;
        return {};
    }

private:
    bool TryZenity(const FileDialogOptions& options, const std::filesystem::path& start) {
        for (const char* program : {"zenity", "zenity3"}) {
            std::vector<std::string> arguments = {program, "--file-selection", "--title=" + options.title};
            if (options.save) arguments.push_back("--save");
            if (!start.empty()) arguments.push_back("--filename=" + start.string());
            for (const FileDialogFilter& filter : options.filters)
                arguments.push_back("--file-filter=" + filter.label + " | " + Joined(Patterns(filter.extensions), ' '));
            const ProcessResult result = Run(arguments);
            if (!result.ran) continue;
            if (result.ok && !result.output.empty()) chosen_ = std::filesystem::path(result.output);
            return true; // the picker ran: an empty answer is a cancel, not a reason to try the next one
        }
        return false;
    }

    bool TryKdialog(const FileDialogOptions& options, const std::filesystem::path& start) {
        std::vector<std::string> arguments = {"kdialog", "--title", options.title,
                                              options.save ? "--getsavefilename" : "--getopenfilename",
                                              start.empty() ? std::string(".") : start.string()};
        std::string filters;
        for (const FileDialogFilter& filter : options.filters) {
            if (!filters.empty()) filters += '|';
            filters += filter.label + " (" + Joined(Patterns(filter.extensions), ' ') + ")";
        }
        if (!filters.empty()) arguments.push_back(filters);
        const ProcessResult result = Run(arguments);
        if (!result.ran) return false;
        if (result.ok) chosen_ = std::filesystem::path(result.output);
        return true;
    }

    bool FindAvailable() const {
        // Round-tripping through Run would launch the picker; ask the filesystem instead.
        const char* path = std::getenv("PATH");
        if (path == nullptr) return found_ = false;
        std::string search = path;
        std::size_t start = 0;
        while (start <= search.size()) {
            const std::size_t end = std::min(search.find(':', start), search.size());
            const std::string folder = search.substr(start, end - start);
            if (!folder.empty() && (access((folder + "/zenity").c_str(), X_OK) == 0 || access((folder + "/zenity3").c_str(), X_OK) == 0 ||
                                    access((folder + "/kdialog").c_str(), X_OK) == 0))
                return found_ = true;
            if (end == search.size()) break;
            start = end + 1;
        }
        return found_ = false;
    }

    std::filesystem::path chosen_;
    mutable std::optional<bool> found_;
};

} // namespace

std::unique_ptr<FileDialog> CreateNativeFileDialog() { return std::make_unique<ZenityFileDialog>(); }

} // namespace aether::editor

#else

namespace aether::editor {
namespace {

class NoFileDialog final : public FileDialog {
public:
    bool Available() const override { return false; }
    std::filesystem::path Show(const FileDialogOptions&) override { return {}; }
};

} // namespace

std::unique_ptr<FileDialog> CreateNativeFileDialog() { return std::make_unique<NoFileDialog>(); }

} // namespace aether::editor

#endif
