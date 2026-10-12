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
#include "aether/ui/draw.h"
#include "aether/ui/input.h"
#include "aether/ui/theme.h"
#include "aether/ui/viewport.h"
#include "aether/ui/widget_system.h"

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
    // The on-screen UI. Layout and painting use the renderer's font, so this one only serves the input router's text metrics.
    ui::BuiltinFont ui_font;
    ui::Viewport ui_viewport;
    std::unique_ptr<ui::UIInputRouter> ui_router;
    std::unique_ptr<ui::UIInputBridge> ui_bridge; // arrows, d-pad, Tab, Enter / Space / A and Escape / B drive the UI
    ui::Theme ui_theme;
    bool ui_has_theme = false;
    std::vector<std::string> ui_images; // the image paths layouts name; a brush's texture id is its index + 1 (0 = none)
    std::unique_ptr<ui::UISystem> ui; // after the viewport it draws on
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
