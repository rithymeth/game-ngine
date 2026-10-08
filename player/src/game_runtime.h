#pragma once

#include "aether/player/game.h"
#include "aether/audio/backend.h"
#include "aether/audio/audio_system.h"
#include "aether/blueprint/system.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/kit/event_bus.h"
#include "aether/sequencer/sequence_system.h"

#include <map>
#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#if AETHER_KIT_QUESTS
#include "aether/quests/quest_system.h"
#endif
#if AETHER_GAME_SCRIPTING
#include "aether/script/script_system.h"
#endif

namespace aether::player {

// Scene-scoped systems share the World, GUID index, and script hosts from Game.
struct Game::Runtime {
    kit::KitEventBus kit_events;
    std::map<std::string, SystemDesc> stages;
    audio::SoundBank sounds;
    std::map<std::string, audio::SoundCue> cues;
    audio::Mixer mixer;
    std::unique_ptr<audio::AudioSystem> audio;
    std::unique_ptr<audio::AudioOutput> output; // stops its thread before audio/mixer/bank destruction
    std::map<std::pair<u32, std::string>, audio::CueHandle> sequence_audio;
    std::unique_ptr<seq::SequenceSystem> sequences;
    std::unique_ptr<gas::AttributeSystem> attributes;
    std::unique_ptr<gas::EffectSystem> effects;
    std::unique_ptr<gas::AbilitySystem> abilities;
#if AETHER_KIT_INVENTORY
    std::unique_ptr<inv::InventorySystem> inventory;
#endif
#if AETHER_KIT_INTERACTION
    std::unique_ptr<interact::InteractionSystem> interaction;
#endif
#if AETHER_KIT_QUESTS
    std::unique_ptr<quest::QuestSystem> quests;
#endif
#if AETHER_GAME_SCRIPTING
    script::LuauHost host;
    std::unique_ptr<script::ScriptSystem> scripts;
#endif
    std::unique_ptr<bp::BlueprintSystem> blueprints;
};

} // namespace aether::player
