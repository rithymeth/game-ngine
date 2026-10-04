# Aether Sprite2D (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.6): the
# 2D toolkit's data and render batching - sprite atlases and animation,
# tilesets and tilemaps with autotiling, and the pixel-perfect camera.
# Engine-only, so it builds and tests headless on every platform.
add_library(aether_sprite2d STATIC
    src/atlas.cpp
    src/components.cpp
    src/tilemap.cpp
    src/batch.cpp
    src/pixel_camera.cpp
    src/physics2d.cpp
    src/platformer.cpp
)
target_include_directories(aether_sprite2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_sprite2d PUBLIC cxx_std_20)
target_link_libraries(aether_sprite2d PUBLIC Aether::Engine)
add_library(Aether::Sprite2D ALIAS aether_sprite2d)
