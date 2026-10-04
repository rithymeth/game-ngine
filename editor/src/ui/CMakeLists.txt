# Portable editor code: widgets built only on Dear ImGui (imgui_core) and the
# engine's reflection system, plus editor core logic such as undo/redo. No
# window or graphics backend, so it builds and runs headless in tests on every
# platform. The Windows editor executable links this library.
add_library(aether_editor_ui STATIC
    reflected_inspector.cpp
    entity_inspector.cpp
    script_inspector.cpp
    error_overlay.cpp
    code_document.cpp
    code_editor.cpp
    ../graph/graph_view.cpp
    ../graph/blueprint_graph.cpp
    ../graph/blueprint_document.cpp
    ../graph/blueprint_editor.cpp
    ../graph/material_document.cpp
    ../graph/material_graph.cpp
    ../graph/material_editor.cpp
    ../anim/anim_document.cpp
    ../anim/anim_graph_editor.cpp
    ../anim/blend_space_editor.cpp
    ../anim/anim_viewer.cpp
    ../audio/waveform.cpp
    ../audio/cue_document.cpp
    ../audio/cue_editor.cpp
    ../audio/mixer_panel.cpp
    ../uidesign/ui_layout_document.cpp
    ../uidesign/ui_designer.cpp
    ../vfx/vfx_document.cpp
    ../vfx/vfx_preview.cpp
    ../vfx/vfx_editor.cpp
    ../ai/bt_document.cpp
    ../ai/bt_editor.cpp
    ../ai/nav_panel.cpp
    ../world/terrain_document.cpp
    ../world/foliage_document.cpp
    ../world/spline_document.cpp
    ../world/world_panels.cpp
    ../net/net_play.cpp
    ../net/net_panels.cpp
    ../devtools/console_panel.cpp
    ../devtools/profiler_panel.cpp
    ../devtools/debug_overlay.cpp
    ../devtools/crash_reporter.cpp
    ../core/command_stack.cpp
    ../core/commands.cpp
    ../core/play_session.cpp
    ../core/recent_projects.cpp
    ../core/json_edit.cpp
    ../host/imgui_window_input.cpp
    ../workspace/editor_workspace.cpp
    ../packaging/build_window.cpp
)
target_include_directories(aether_editor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(aether_editor_ui PUBLIC Aether::Engine Aether::Blueprint Aether::Renderer Aether::Animation Aether::Audio Aether::UI Aether::VFX Aether::AI Aether::Terrain Aether::Streaming Aether::Net imgui_core)
add_library(Aether::EditorUI ALIAS aether_editor_ui)

# Phase 24 step 5: Dear ImGui on the RHI's Vulkan backend (imgui_impl_vulkan
# on the engine's own device), for the portable editor shell and its tests.
if(AETHER_BUILD_VULKAN)
    add_library(aether_editor_vulkan STATIC
        ../host/imgui_vulkan_host.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(aether_editor_vulkan PUBLIC ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(aether_editor_vulkan PUBLIC aether_editor_ui PRIVATE ${AETHER_VULKAN_LIBRARY})
    # Third-party code: don't hold it to our warning flags.
    if(MSVC)
        set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_OPTIONS /w)
    else()
        set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_OPTIONS -w)
        set_source_files_properties(../host/imgui_vulkan_host.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    add_library(Aether::EditorVulkan ALIAS aether_editor_vulkan)
endif()
