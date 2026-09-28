# Aether AI (Phase 20, docs/design/PHASE_SPECS.md): Blackboards and Behavior
# Trees, driving NavAgents. Engine-only, so it builds and tests headless on
# every platform; Blueprint and Luau tasks plug in through hooks.
add_library(aether_ai STATIC
    src/blackboard.cpp
    src/behavior_tree.cpp
    src/bt_runtime.cpp
    src/bt_world.cpp
)
target_include_directories(aether_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_ai PUBLIC cxx_std_20)
target_link_libraries(aether_ai PUBLIC Aether::Engine Aether::Nav)
add_library(Aether::AI ALIAS aether_ai)
