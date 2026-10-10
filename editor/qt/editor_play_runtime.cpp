#include "editor_play_runtime.h"

#include "aether/assets/asset_database.h"
#include "aether/core/log.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics/physics_world.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#if defined(AETHER_QT_EDITOR_SCRIPTING)
#include "aether/script/luau_host.h"
#include "aether/script/script_system.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace aether::editor::qt {

namespace {

void LoadInputAssets(const std::filesystem::path& content_root, input::InputAssetLibrary& library) {
    if (content_root.empty()) return;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        content_root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    const auto load_entry = [&](const std::filesystem::directory_entry& entry) {
        const std::string filename = entry.path().filename().string();
        std::error_code status_error;
        const std::filesystem::file_status status = entry.symlink_status(status_error);
        if (status_error) return;
        if (std::filesystem::is_directory(status)) {
            if (!filename.empty() && filename.front() == '.') it.disable_recursion_pending();
            return;
        }
        if (!std::filesystem::is_regular_file(status)) return;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const char* importer = extension == ".aaction" ? "InputAction"
            : extension == ".amapping" ? "InputMapping" : nullptr;
        if (!importer) return;

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file) {
            AETHER_LOG_WARN("QtEditor", "Couldn't read input asset %s", entry.path().string().c_str());
            return;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string relative = entry.path().lexically_relative(content_root).generic_string();
        library.AddFromText(importer, relative, text);
    };
    while (it != end) {
        load_entry(*it);
        it.increment(error);
        if (error) error.clear();
    }
}

} // namespace

EditorPlayRuntime::EditorPlayRuntime(World& world, GuidIndex& guids, const std::filesystem::path& content_root,
                                     const ProjectSettings& project)
    : world_(world), fixed_step_(project.fixed_timestep_hz > 0.0f ? 1.0f / project.fixed_timestep_hz : 1.0f / 60.0f) {
    jobs_ = std::make_unique<JobSystem>();
    physics_ = std::make_unique<PhysicsWorld>(*jobs_);
    physics_->SetGravity(project.gravity);
    physics_->SetCollisionMatrix(MakeCollisionMatrix(project));
    scene_ = std::make_unique<PhysicsScene>(world_, *physics_);
    characters_ = std::make_unique<CharacterSystem>(world_, *physics_, scene_.get());
    lifecycle_ = std::make_unique<Lifecycle>(world_, guids);

    LoadInputAssets(content_root, input_library_);
    for (const std::string& warning : input_library_.Errors())
        AETHER_LOG_WARN("QtEditor", "%s", warning.c_str());
    input_library_.RegisterActions(input_);
    i32 input_priority = 0;
    for (const std::string& name : input_library_.ContextNames())
        input_library_.Activate(input_, name, input_priority++);

#if defined(AETHER_QT_EDITOR_SCRIPTING)
    if (!content_root.empty()) {
        assets_ = std::make_unique<assets::AssetDatabase>(content_root);
        const assets::ScanResult scan = assets_->Scan();
        for (const std::string& warning : scan.warnings)
            AETHER_LOG_WARN("QtEditor", "%s", warning.c_str());
    }
    host_ = std::make_unique<script::LuauHost>();
    scripts_ = std::make_unique<script::ScriptSystem>(
        *host_, world_, guids,
        [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            if (!assets_) return false;
            const assets::AssetRecord* record = assets_->Find(guid);
            if (!record || record->missing || record->importer != assets::ScriptAsset::kImporter) return false;
            const std::filesystem::path path = assets_->SourcePath(guid);
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            name = record->path;
            return true;
        }, false);
    scripts_->Register(*lifecycle_);
    scripts_->BindInput(&input_);
    scripts_->BindPhysicsScene(scene_.get());
    scene_->SetEventHandler([this](const PhysicsEvent& event) {
        if (!scripts_) return;
        const char* event_name = PhysicsEventName(event.type);
        constexpr char prefix[] = "Event.";
        const std::string full_name = event_name ? event_name : "";
        const std::string method = full_name.rfind(prefix, 0) == 0 ? full_name.substr(sizeof(prefix) - 1) : full_name;
        if (!method.empty()) {
            scripts_->SendEvent(event.self, method,
                {script::EntityRef{event.other}, static_cast<f64>(event.approach_speed)});
        }
    });
#else
    (void)content_root;
    (void)guids;
#endif

    scene_->Sync();
    lifecycle_->BeginPlay();
}

EditorPlayRuntime::~EditorPlayRuntime() { Stop(); }

void EditorPlayRuntime::Tick(f32 dt) {
    if (stopped_) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    input_.Update(input_state_, dt);
    lifecycle_->Update(dt);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    if (scripts_) scripts_->Tick(dt);
#endif

    accumulator_ += dt;
    constexpr int kMaxFixedStepsPerFrame = 8;
    int steps = 0;
    while (accumulator_ >= fixed_step_ && steps < kMaxFixedStepsPerFrame) {
        characters_->Step(fixed_step_);
        scene_->Step(fixed_step_);
        lifecycle_->FixedUpdate(fixed_step_);
        accumulator_ -= fixed_step_;
        ++steps;
    }
    if (steps == kMaxFixedStepsPerFrame && accumulator_ >= fixed_step_)
        accumulator_ = std::fmod(accumulator_, fixed_step_);
    lifecycle_->LateUpdate(dt);
    input_state_.EndFrame();
}

void EditorPlayRuntime::SetButton(input::Key key, bool down) {
    if (!stopped_) input_state_.SetButton(key, down);
}

void EditorPlayRuntime::AddMouseDelta(f32 dx, f32 dy) {
    if (!stopped_) input_state_.AddMouseDelta(dx, dy);
}

void EditorPlayRuntime::AddMouseWheel(f32 delta) {
    if (!stopped_) input_state_.AddWheel(delta);
}

void EditorPlayRuntime::ClearInput() {
    input_state_.Clear();
    input_.Reset();
}

void EditorPlayRuntime::Stop() {
    if (stopped_) return;
    stopped_ = true;
    ClearInput();
    if (lifecycle_ && lifecycle_->IsPlaying()) lifecycle_->EndPlay();
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    if (scripts_) scripts_->BindInput(nullptr);
#endif
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    scripts_.reset();
    host_.reset();
    assets_.reset();
#endif
    lifecycle_.reset();
    characters_.reset();
    scene_.reset();
    physics_.reset();
    jobs_.reset();
}

} // namespace aether::editor::qt
