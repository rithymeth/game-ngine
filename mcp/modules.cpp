#include "editor_tools.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/script_component.h"

#include "aether/animation/animator.h"
#include "aether/audio/audio_system.h"
#include "aether/blueprint/system.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/nav/components.h"
#include "aether/nav/crowd.h"
#include "aether/renderer/components.h"
#include "aether/sequencer/sequence_system.h"
#include "aether/sprite2d/lights2d.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"
#include "aether/streaming/partition.h"
#include "aether/ui/widget_system.h"
#include "aether/ui/world_ui.h"
#include "aether/vfx/particle_system.h"

#if AETHER_MCP_PHYSICS
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#endif
#if AETHER_MCP_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_MCP_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#if AETHER_MCP_QUESTS
#include "aether/quests/quest_system.h"
#endif

// Note: no Aether::AI (behaviour trees, perception) -- the editor/MCP side
// must not link gameplay AI (docs/design/agent_gameplay_ai_boundary.md).

namespace aether::mcp {

void RegisterBuiltinComponents() {
    // Scene core.
    GetComponentId<EntityName>();
    GetComponentId<Transform>();
    GetComponentId<Parent>();
    GetComponentId<IdComponent>();
    GetComponentId<ModelRenderer>();
    GetComponentId<Active>();
    GetComponentId<Camera>();
    GetComponentId<CineCamera>();
    GetComponentId<Tags>();
    GetComponentId<Layer>();
    GetComponentId<ScriptComponent>();

    // Module components.
    RegisterAnimationComponents();
    RegisterAudioComponents();
    bp::RegisterBlueprintComponents();
    gas::RegisterGameplayComponents();
    RegisterNavComponents();
    RegisterNavAgentComponents();
    RegisterRenderComponents();
    RegisterSequenceComponents();
    sprite2d::RegisterPlatformerComponents();
    GetComponentId<sprite2d::Rigidbody2D>();
    GetComponentId<sprite2d::Collider2D>();
    GetComponentId<sprite2d::Light2D>();
    GetComponentId<sprite2d::ShadowCaster2D>();
    RegisterStreamingComponents();
    ui::RegisterWidgetComponents();
    ui::RegisterWorldWidgetComponents();
    RegisterParticleComponents();

#if AETHER_MCP_PHYSICS
    RegisterPhysicsComponentSerializers();
    GetComponentId<RigidBody>();
    GetComponentId<BoxCollider>();
    GetComponentId<SphereCollider>();
    GetComponentId<CapsuleCollider>();
    GetComponentId<ConvexCollider>();
    GetComponentId<MeshCollider>();
    GetComponentId<CharacterMovement>();
#endif
#if AETHER_MCP_INVENTORY
    inv::RegisterInventoryComponents();
#endif
#if AETHER_MCP_INTERACTION
    interact::RegisterInteractionComponents();
#endif
#if AETHER_MCP_QUESTS
    quest::RegisterQuestComponents();
#endif
}

} // namespace aether::mcp
