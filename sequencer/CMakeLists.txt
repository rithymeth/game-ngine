# Aether Sequencer (Phase 27, docs/design/PHASE_SPECS.md §27): cinematics.
# A level sequence is tracks of keyed values bound to entities; the player
# applies them to a world. Engine-only, so it builds and tests headless on
# every platform.
add_library(aether_sequencer STATIC
    src/sequence.cpp
    src/player.cpp
)
target_include_directories(aether_sequencer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_sequencer PUBLIC cxx_std_20)
target_link_libraries(aether_sequencer PUBLIC Aether::Engine)
add_library(Aether::Sequencer ALIAS aether_sequencer)
