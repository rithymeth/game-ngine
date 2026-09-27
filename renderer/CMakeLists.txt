# Aether Renderer (Phase 14, docs/design/PHASE_SPECS.md): the renderer's
# CPU side (render scene extraction, views, culling, draw lists, and the
# CPU references for the GPU passes). Engine-only, so it builds and tests
# headless on every platform; the GPU passes on the RHI come later.
add_library(aether_renderer STATIC
    src/components.cpp
    src/render_scene.cpp
    src/view.cpp
    src/draw_list.cpp
    src/clusters.cpp
    src/shadows.cpp
    src/frame_graph.cpp
    src/post.cpp
    src/material.cpp
    src/material_codegen.cpp
)
target_include_directories(aether_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_renderer PUBLIC cxx_std_20)
target_link_libraries(aether_renderer PUBLIC Aether::Engine)
add_library(Aether::Renderer ALIAS aether_renderer)
