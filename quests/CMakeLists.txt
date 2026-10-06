# Aether Quests (Phase 30 step 7c, docs/design/PHASE_SPECS.md §30.10): the quests
# genre kit, built on the gameplay module's effects. Optional: AETHER_KIT_QUESTS=OFF
# leaves it out of the build (and of the player). It doesn't depend on the other kits:
# the player forwards item changes into it, and scripts and Blueprints notify it.
add_library(aether_quests STATIC
    src/quest_def.cpp
    src/quest_system.cpp
    src/quests_kit.cpp
    src/quest_library.cpp
)
target_include_directories(aether_quests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_quests PUBLIC cxx_std_20)
target_link_libraries(aether_quests PUBLIC Aether::Gameplay Aether::Loc)
aether_warnings_as_errors(aether_quests)
add_library(Aether::Quests ALIAS aether_quests)
