# Aether Animation (Phase 16, docs/design/PHASE_SPECS.md): skeletons, poses,
# clips, blending, blend spaces, animation graphs, IK and retargeting.
# Engine-only, so it builds and tests headless on every platform.
add_library(aether_animation STATIC
    src/skeleton.cpp
    src/clip.cpp
    src/compression.cpp
)
target_include_directories(aether_animation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_animation PUBLIC cxx_std_20)
target_link_libraries(aether_animation PUBLIC Aether::Engine)
add_library(Aether::Animation ALIAS aether_animation)
