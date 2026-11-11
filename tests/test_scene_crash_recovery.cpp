#include "core/scene_document.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

// Roadmap row A03: terminate the editor during an unsaved edit and recover without overwriting the last saved scene. The in-process half is in test_scene_document.cpp; this is the half
// that needs a real process death: a child process saves the scene and its autosave in a tight loop and is killed with SIGKILL at a random moment, over and over. Whenever it dies,
// the last saved scene, its backup and the autosave must each be whole files that load; never a truncated one.

#if !defined(_WIN32)
namespace {

constexpr usize kOldNamed = 40, kUnsaved = 1500; // entities made by the test; a new scene also has a few of its own, so the counts the test compares are measured, not assumed

// The scene as it was last saved (kOldEntities) and, with unsaved work on top, as the child keeps writing it (kOldEntities + 1, with a lot of text so a write takes a few ms).
void BuildOld(SceneDocument& document) {
    document.NewScene();
    for (usize i = 0; i < kOldNamed; ++i) document.CreateEntity("Old " + std::to_string(i));
}
void AddUnsavedWork(SceneDocument& document) {
    for (usize i = 0; i < kUnsaved; ++i) {
        const Entity e = document.CreateEntity("Unsaved " + std::to_string(i) + std::string(40, 'x'));
        document.SetPosition(e, Vec3{static_cast<f32>(i), 1.0f, 2.0f}, true);
    }
}

} // namespace

// Not a test of its own: the child process the crash test starts (env AETHER_CRASH_CHILD_DIR names the folder). Without the variable it does nothing.
AETHER_TEST(SceneCrash_ChildWriterForTheKillTest) {
    const char* dir = std::getenv("AETHER_CRASH_CHILD_DIR");
    if (dir == nullptr) return;
    const stdfs::path root(dir);
    SceneDocument document;
    std::string error;
    if (!document.Load(root / "level.ascene", &error)) std::_Exit(3);
    AddUnsavedWork(document);
    if (std::getenv("AETHER_CRASH_CHILD_NAIVE") != nullptr) {
        // The control: a writer that truncates the file and writes into it (what a save without the temp file and the rename would be). Killed mid-write it leaves a short file.
        document.SaveAutosave(root / "naive_source.ascene", &error);
        std::ifstream in(root / "naive_source.ascene", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        { std::ofstream(root / "ready") << "1"; }
        for (;;) {
            std::ofstream out(root / "Autosaves" / "level.ascene", std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
    }
    { std::ofstream(root / "ready") << "1"; }
    for (;;) { // until killed
        document.SaveAutosave(root / "Autosaves" / "level.ascene", &error);
        document.Save(root / "level.ascene", &error);
    }
}

struct KillStats {
    int kills = 0, mid_write = 0, broken = 0, autosave_missing = 0;
    long old_seen = 0, new_seen = 0;
};

// Starts a child that writes the scene over and over, kills it `rounds` times at random moments (stopping early at the first broken file when `stop_at_first_broken`), and checks what it left behind.
KillStats KillTheWriter(const stdfs::path& root, bool naive, int rounds, bool stop_at_first_broken) {
    KillStats stats;
    std::error_code ignored;
    stdfs::remove_all(root, ignored);
    stdfs::create_directories(root / "Autosaves");
    const stdfs::path level = root / "level.ascene", autosave = root / "Autosaves" / "level.ascene";

    auto restore_old = [&] { // the last saved scene, before the unsaved work
        stdfs::remove(root / "ready", ignored);
        stdfs::remove(level, ignored);
        stdfs::remove(autosave, ignored);
        stdfs::remove(level.string() + ".bak", ignored);
        SceneDocument document;
        BuildOld(document);
        std::string error;
        AETHER_CHECK(document.Save(level, &error));
    };
    // How many entities the file at `path` has, or -1 if it is missing or does not load.
    auto entities_in = [&](const stdfs::path& path) -> long {
        if (!stdfs::exists(path)) return -2;
        stdfs::path loadable = path; // a .bak is a scene under another name: the loader goes by the extension
        if (path.extension() == ".bak") { loadable = root / "backup_copy.ascene"; stdfs::copy_file(path, loadable, stdfs::copy_options::overwrite_existing, ignored); }
        SceneDocument document;
        std::string error;
        if (!document.Load(loadable, &error)) return -1;
        return static_cast<long>(document.Entities().size());
    };

    usize old_count = 0;
    { SceneDocument probe; BuildOld(probe); old_count = probe.Entities().size(); }
    const long old_n = static_cast<long>(old_count), new_n = static_cast<long>(old_count + kUnsaved);
    const std::string self = "/proc/self/exe";
    unsigned seed = 12345;
    for (int round = 0; round < rounds; ++round) {
        restore_old();
        // The child's environment only: the test framework reads AETHER_TEST_FILTER as it goes, so setting it here would change what the rest of this run does.
        std::vector<std::string> env_storage;
        for (char** e = environ; *e != nullptr; ++e) {
            const std::string entry = *e;
            if (entry.rfind("AETHER_TEST_FILTER=", 0) != 0) env_storage.push_back(entry);
        }
        env_storage.push_back("AETHER_CRASH_CHILD_DIR=" + root.string());
        if (naive) env_storage.push_back("AETHER_CRASH_CHILD_NAIVE=1");
        env_storage.push_back("AETHER_TEST_FILTER=SceneCrash_ChildWriterForTheKillTest");
        std::vector<char*> envp;
        for (std::string& entry : env_storage) envp.push_back(entry.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        char* args[] = {const_cast<char*>(self.c_str()), nullptr};
        pid_t child = 0;
        const int spawned = posix_spawn(&child, self.c_str(), &actions, nullptr, args, envp.data());
        posix_spawn_file_actions_destroy(&actions);
        AETHER_CHECK(spawned == 0);
        if (spawned != 0) break;
        // Wait until it has loaded the scene and started writing (or died), then let it write for a random while.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!stdfs::exists(root / "ready") && std::chrono::steady_clock::now() < give_up) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        AETHER_CHECK(stdfs::exists(root / "ready"));
        seed = seed * 1664525u + 1013904223u;
        std::this_thread::sleep_for(std::chrono::microseconds(500 + (seed >> 8) % 30000)); // 0.5 to 30 ms
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        ++stats.kills;
        const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        AETHER_CHECK(killed);
        // What it left behind: a temp file means the kill landed mid-write.
        if (stdfs::exists(level.string() + ".tmp") || stdfs::exists(autosave.string() + ".tmp") || stdfs::exists(level.string() + ".bak.tmp")) ++stats.mid_write;
        const long saved = entities_in(level), recovered = entities_in(autosave), backup = entities_in(level.string() + ".bak");
        // The last saved scene is whole: the old one, or the new one if the kill came after the swap. Never missing, never truncated.
        if (saved != old_n && saved != new_n) { ++stats.broken; std::printf("    round %d: the saved scene has %ld entities (missing -2, unloadable -1)\n", round, saved); }
        // The autosave, if it was written at all, is whole too (the writer replaces it atomically).
        if (recovered == -2) ++stats.autosave_missing;
        else if (recovered != new_n) { ++stats.broken; std::printf("    round %d: the autosave has %ld entities\n", round, recovered); }
        // The backup, if there is one, is the old scene whole.
        if (backup != -2 && backup != old_n && backup != new_n) { ++stats.broken; std::printf("    round %d: the backup has %ld entities\n", round, backup); }
        (saved == old_n ? stats.old_seen : stats.new_seen)++;
        if (stop_at_first_broken && stats.broken > 0) break;
    }
    return stats;
}

AETHER_TEST(SceneCrash_AKilledEditorLeavesTheSavedSceneAndTheAutosaveWhole) {
    const stdfs::path root = stdfs::temp_directory_path() / "aether_scene_crash" / std::to_string(::getpid());
    const KillStats stats = KillTheWriter(root, false, 100, false);
    std::printf("    %d kills: %d left a temp file (died mid-write), saved scene still the old one %ld times and the new one %ld times, autosave not yet written %d times, broken files %d\n", stats.kills,
                stats.mid_write, stats.old_seen, stats.new_seen, stats.autosave_missing, stats.broken);
    AETHER_CHECK(stats.kills >= 90);
    AETHER_CHECK(stats.broken == 0);
    const stdfs::path level = root / "level.ascene", autosave = root / "Autosaves" / "level.ascene";
    std::error_code ignored;
    // And what the editor does next: recover the autosave over the saved scene's name without touching the file until saved.
    {
        stdfs::remove(autosave, ignored);
        stdfs::remove(level, ignored);
        SceneDocument fresh;
        BuildOld(fresh);
        AETHER_CHECK(fresh.Save(level));
    }
    {
        SceneDocument edited;
        AETHER_CHECK(edited.Load(level));
        AddUnsavedWork(edited);
        std::string error;
        AETHER_CHECK(edited.SaveAutosave(autosave, &error));
    } // "the editor dies"
    usize old_count = 0;
    { SceneDocument probe; BuildOld(probe); old_count = probe.Entities().size(); }
    SceneDocument recovered;
    std::string error;
    AETHER_CHECK(recovered.LoadAutosave(autosave, level, &error) && recovered.Entities().size() == old_count + kUnsaved && recovered.IsDirty());
    SceneDocument on_disk;
    AETHER_CHECK(on_disk.Load(level) && on_disk.Entities().size() == old_count); // the saved scene was not overwritten by recovering
    stdfs::remove_all(root, ignored);
}

// The control: the same kill loop against a writer that is not atomic must produce broken files, or the test above could not have failed.
AETHER_TEST(SceneCrash_TheKillTestCatchesAWriterThatIsNotAtomic) {
    const stdfs::path root = stdfs::temp_directory_path() / "aether_scene_crash_naive" / std::to_string(::getpid());
    const KillStats stats = KillTheWriter(root, true, 80, true);
    std::printf("    the naive writer: %d kills until a broken file turned up, broken files %d\n", stats.kills, stats.broken);
    AETHER_CHECK(stats.broken > 0);
    std::error_code ignored;
    stdfs::remove_all(root, ignored);
}
#endif
