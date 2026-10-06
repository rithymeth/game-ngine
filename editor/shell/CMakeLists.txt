add_executable(aether_editor_shell main.cpp)
target_link_libraries(aether_editor_shell PRIVATE Aether::EditorVulkan Aether::EditorUI)
target_include_directories(aether_editor_shell SYSTEM PRIVATE ${stb_SOURCE_DIR})
aether_link_platform_plugins(aether_editor_shell)
