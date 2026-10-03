#pragma once

#include "aether/debug/crash.h"

#include <string>
#include <vector>

namespace aether::editor {

// The crash reporter (Phase 23 step 4): on startup, if the last runs left
// crash reports the user hasn't seen, a window lists them; for the one
// selected it shows the reason, build, time, the game's context, the
// backtrace and the last log lines. Copy puts the whole report on the
// clipboard; Dismiss marks it seen; Delete removes it (and its minidump).
// Portable; draws its own window.
class CrashReporterDialog {
public:
    explicit CrashReporterDialog(std::string directory) : directory_(std::move(directory)) {}

    // Loads the unseen reports; opens when there are any.
    void Refresh();
    void Draw();
    bool open = false;

    const std::vector<CrashReport>& Reports() const { return reports_; }
    int selected = 0;
    bool Dismiss(usize index);
    bool Delete(usize index);
    void DismissAll();
    std::string ReportText(usize index) const; // the file, for the clipboard

private:
    void Remove(usize index);
    std::string directory_;
    std::vector<CrashReport> reports_;
};

} // namespace aether::editor
