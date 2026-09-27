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
    ../core/command_stack.cpp
    ../core/commands.cpp
    ../core/play_session.cpp
    ../core/recent_projects.cpp
)
target_include_directories(aether_editor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(aether_editor_ui PUBLIC Aether::Engine imgui_core)
add_library(Aether::EditorUI ALIAS aether_editor_ui)
