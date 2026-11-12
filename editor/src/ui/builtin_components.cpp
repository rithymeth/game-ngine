#include "ui/builtin_components.h"

#include "aether/scene/components.h"
#include "aether/scene/data_layers.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/level_instance.h"
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

#if AETHER_EDITOR_NET_PHYSICS
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#endif
#if AETHER_EDITOR_KIT_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_EDITOR_KIT_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#if AETHER_EDITOR_KIT_QUESTS
#include "aether/quests/quest_system.h"
#endif

namespace aether::editor {

void RegisterEditorComponents() {
    // Scene core.
    (void)GetComponentId<EntityName>();
    (void)GetComponentId<Transform>();
    (void)GetComponentId<Parent>();
    (void)GetComponentId<IdComponent>();
    (void)GetComponentId<ModelRenderer>();
    (void)GetComponentId<Active>();
    (void)GetComponentId<Camera>();
    (void)GetComponentId<CineCamera>();
    (void)GetComponentId<Tags>();
    (void)GetComponentId<Layer>();
    (void)GetComponentId<ScriptComponent>();
    (void)GetComponentId<DataLayer>();
    (void)GetComponentId<LevelInstance>();

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
    (void)GetComponentId<sprite2d::Rigidbody2D>();
    (void)GetComponentId<sprite2d::Collider2D>();
    (void)GetComponentId<sprite2d::Light2D>();
    (void)GetComponentId<sprite2d::ShadowCaster2D>();
    RegisterStreamingComponents();
    ui::RegisterWidgetComponents();
    ui::RegisterWorldWidgetComponents();
    RegisterParticleComponents();

#if AETHER_EDITOR_NET_PHYSICS
    RegisterPhysicsComponentSerializers();
    (void)GetComponentId<RigidBody>();
    (void)GetComponentId<BoxCollider>();
    (void)GetComponentId<SphereCollider>();
    (void)GetComponentId<CapsuleCollider>();
    (void)GetComponentId<ConvexCollider>();
    (void)GetComponentId<MeshCollider>();
    (void)GetComponentId<HeightfieldCollider>();
    (void)GetComponentId<CharacterMovement>();
#endif
#if AETHER_EDITOR_KIT_INVENTORY
    inv::RegisterInventoryComponents();
#endif
#if AETHER_EDITOR_KIT_INTERACTION
    interact::RegisterInteractionComponents();
#endif
#if AETHER_EDITOR_KIT_QUESTS
    quest::RegisterQuestComponents();
#endif
}

} // namespace aether::editor
