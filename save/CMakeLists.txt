# Aether Save (Phase 28, docs/design/PHASE_SPECS.md §28): save slots.
# A game's reflected struct, saved to a slot as a versioned, checksummed
# .asav file with an atomic write. Engine-only, so it builds and tests
# headless on every platform.
add_library(aether_save STATIC
    src/envelope.cpp
    src/save_system.cpp
    src/world_state.cpp
    src/settings.cpp
)
target_include_directories(aether_save PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_save PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(aether_save PUBLIC Aether::Engine Threads::Threads)
add_library(Aether::Save ALIAS aether_save)
