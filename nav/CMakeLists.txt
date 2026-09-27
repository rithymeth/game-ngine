# Aether Nav (Phase 20, docs/design/PHASE_SPECS.md): navigation meshes baked
# with Recast, paths and queries with Detour. Engine-only, so it builds and
# tests headless on every platform.
add_library(aether_nav STATIC
    src/navmesh.cpp
)
target_include_directories(aether_nav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_nav PUBLIC cxx_std_20)
target_link_libraries(aether_nav PUBLIC Aether::Engine PRIVATE RecastNavigation::Recast RecastNavigation::Detour)
add_library(Aether::Nav ALIAS aether_nav)
