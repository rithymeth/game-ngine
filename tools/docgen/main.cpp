// aether_docgen: writes the API reference from the reflection registry
// (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.7).
//
//   aether_docgen [--out <dir>] [--stdout] [--title <text>] [--with-scalars]
//
// With no --out it prints the Markdown. A type is in the reference when its
// module is linked into this tool and registered, so the engine's modules are
// all included here; a game builds its own tool the same way, or calls
// aether::docs::WriteApiDocs after its types are registered.
#include "aether/docs/api_docs.h"

#include "aether/ai/bt_world.h"
#include "aether/ai/perception.h"
#include "aether/animation/animator.h"
#include "aether/audio/audio_system.h"
#include "aether/blueprint/system.h"
#include "aether/nav/components.h"
#include "aether/nav/crowd.h"
#include "aether/net/prediction.h"
#include "aether/renderer/components.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/lights2d.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"
#include "aether/streaming/partition.h"
#include "aether/ui/widget_system.h"
#include "aether/ui/world_ui.h"
#include "aether/vfx/particle_system.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/prefab.h"
#include "aether/scene/script_component.h"
#ifdef AETHER_DOCGEN_PHYSICS
#include "aether/physics/components.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>

using namespace aether;

namespace {

void RegisterEverything() {
    (void)GetComponentId<Transform>();
    (void)GetComponentId<ModelRenderer>();
    (void)GetComponentId<Camera>();
    (void)GetComponentId<Tags>();
    (void)GetComponentId<Layer>();
    (void)GetComponentId<ScriptComponent>();
    bp::RegisterBlueprintComponents();
    RegisterRenderComponents();
    RegisterAudioComponents();
    ui::RegisterWidgetComponents();
    ui::RegisterWorldWidgetComponents();
    RegisterParticleComponents();
    RegisterBehaviorTreeComponents();
    RegisterPerceptionComponents();
    RegisterNavComponents();
    RegisterNavAgentComponents();
    RegisterStreamingComponents();
    sprite2d::RegisterSprite2DComponents();
    sprite2d::RegisterPhysics2DComponents();
    sprite2d::RegisterPlatformerComponents();
    sprite2d::RegisterLight2DComponents();
#ifdef AETHER_DOCGEN_PHYSICS
    (void)GetComponentId<RigidBody>();
    (void)GetComponentId<BoxCollider>();
    (void)GetComponentId<SphereCollider>();
    (void)GetComponentId<CapsuleCollider>();
#endif
}

int Usage() {
    std::fprintf(stderr, "usage: aether_docgen [--out <dir>] [--title <text>] [--with-scalars]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string out;
    docs::ApiDocOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--title" && i + 1 < argc) options.title = argv[++i];
        else if (a == "--with-scalars") options.skip_scalars = false;
        else return Usage();
    }
    RegisterEverything();
    if (out.empty()) {
        std::fputs(docs::GenerateApiMarkdown(options).c_str(), stdout);
        return 0;
    }
    std::string error;
    if (!docs::WriteApiDocs(out, options, &error)) {
        std::fprintf(stderr, "aether_docgen: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "aether_docgen: wrote %s/API.md and api.json\n", out.c_str());
    return 0;
}
