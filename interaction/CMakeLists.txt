# Aether Interaction (Phase 30 step 7b, docs/design/PHASE_SPECS.md §30.8): the
# interaction genre kit (doors, levers, pickups, anything you walk up to and use).
# Optional: AETHER_KIT_INTERACTION=OFF leaves it out of the build (and of the player).
add_library(aether_interaction STATIC
    src/interactable.cpp
    src/interaction_system.cpp
    src/interaction_library.cpp
)
target_include_directories(aether_interaction PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_interaction PUBLIC cxx_std_20)
target_link_libraries(aether_interaction PUBLIC Aether::Gameplay Aether::Loc)
aether_warnings_as_errors(aether_interaction)
add_library(Aether::Interaction ALIAS aether_interaction)
