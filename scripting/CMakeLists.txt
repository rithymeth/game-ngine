add_library(aether_scripting STATIC
    src/luau_host.cpp
    src/world_bindings.cpp
    src/script_system.cpp
    src/script_api.cpp
    src/script_reload.cpp
    src/debugger.cpp
    src/completion.cpp
    src/dap.cpp
    src/debug_bindings.cpp
    src/save_api.cpp
    src/loc_api.cpp
    src/ui_api.cpp
    src/gameplay_api.cpp
    src/inventory_api.cpp
    src/interaction_api.cpp
    src/quests_api.cpp
    src/crafting_api.cpp
    src/loot_api.cpp
    src/dialogue_api.cpp
    src/timeofday_api.cpp
    src/achievements_api.cpp
    src/spawner_api.cpp
    src/checkpoints_api.cpp
    src/forces_api.cpp
    src/destruction_api.cpp
    src/vehicles_api.cpp
    src/ropes_api.cpp
    src/ai_api.cpp
)

target_include_directories(aether_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_scripting PUBLIC cxx_std_20)
# Luau's headers stay private: engine code talks to LuauHost, not to lua.h.
target_link_libraries(aether_scripting PUBLIC Aether::Engine PRIVATE Luau.VM Luau.Compiler Aether::Audio Aether::Save Aether::Loc Aether::Gameplay Aether::UI Aether::AI)
if(AETHER_BUILD_PHYSICS)
    target_link_libraries(aether_scripting PRIVATE Aether::Physics)
    target_compile_definitions(aether_scripting PRIVATE AETHER_SCRIPTING_PHYSICS=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_SCRIPTING_PHYSICS=0)
endif()

if(TARGET Aether::Inventory)
    target_link_libraries(aether_scripting PRIVATE Aether::Inventory)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_INVENTORY=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_INVENTORY=0)
endif()
if(TARGET Aether::Interaction)
    target_link_libraries(aether_scripting PRIVATE Aether::Interaction)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_INTERACTION=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_INTERACTION=0)
endif()
if(TARGET Aether::Quests)
    target_link_libraries(aether_scripting PRIVATE Aether::Quests)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_QUESTS=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_QUESTS=0)
endif()
if(TARGET Aether::ForceFields)
    target_link_libraries(aether_scripting PRIVATE Aether::ForceFields)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_FORCEFIELDS=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_FORCEFIELDS=0)
endif()
if(TARGET Aether::Destruction)
    target_link_libraries(aether_scripting PRIVATE Aether::Destruction)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_DESTRUCTION=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_DESTRUCTION=0)
endif()
if(TARGET Aether::Vehicles)
    target_link_libraries(aether_scripting PRIVATE Aether::Vehicles)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_VEHICLES=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_VEHICLES=0)
endif()
if(TARGET Aether::Ropes)
    target_link_libraries(aether_scripting PRIVATE Aether::Ropes)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_ROPES=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_ROPES=0)
endif()
if(TARGET Aether::Checkpoints)
    target_link_libraries(aether_scripting PRIVATE Aether::Checkpoints)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_CHECKPOINTS=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_CHECKPOINTS=0)
endif()
if(TARGET Aether::Spawner)
    target_link_libraries(aether_scripting PRIVATE Aether::Spawner)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_SPAWNER=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_SPAWNER=0)
endif()
if(TARGET Aether::Achievements)
    target_link_libraries(aether_scripting PRIVATE Aether::Achievements)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_ACHIEVEMENTS=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_ACHIEVEMENTS=0)
endif()
if(TARGET Aether::TimeOfDay)
    target_link_libraries(aether_scripting PRIVATE Aether::TimeOfDay)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_TIMEOFDAY=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_TIMEOFDAY=0)
endif()
if(TARGET Aether::Dialogue)
    target_link_libraries(aether_scripting PRIVATE Aether::Dialogue)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_DIALOGUE=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_DIALOGUE=0)
endif()
if(TARGET Aether::Loot)
    target_link_libraries(aether_scripting PRIVATE Aether::Loot)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_LOOT=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_LOOT=0)
endif()
if(TARGET Aether::Crafting)
    target_link_libraries(aether_scripting PRIVATE Aether::Crafting)
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_CRAFTING=1)
else()
    target_compile_definitions(aether_scripting PRIVATE AETHER_KIT_CRAFTING=0)
endif()
add_library(Aether::Scripting ALIAS aether_scripting)
