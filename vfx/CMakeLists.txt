# Aether VFX (Phase 19, docs/design/PHASE_SPECS.md): particle systems -
# emitter module stacks, curves and gradients, the CPU simulation, and draw
# data for the renderer.
# Engine-only, so it builds and tests headless on every platform.
add_library(aether_vfx STATIC
    src/curves.cpp
    src/emitter.cpp
    src/simulation.cpp
    src/render.cpp
    src/collision.cpp
    src/particle_system.cpp
)
target_include_directories(aether_vfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_vfx PUBLIC cxx_std_20)
target_link_libraries(aether_vfx PUBLIC Aether::Engine)
add_library(Aether::VFX ALIAS aether_vfx)
