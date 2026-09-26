# Portable editor UI: widgets built only on Dear ImGui (imgui_core) and the
# engine's reflection system, with no window or graphics backend, so they
# build and run headless in tests on every platform. The Windows editor
# executable links this library.
add_library(aether_editor_ui STATIC
    reflected_inspector.cpp
)
target_include_directories(aether_editor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(aether_editor_ui PUBLIC Aether::Engine imgui_core)
add_library(Aether::EditorUI ALIAS aether_editor_ui)
