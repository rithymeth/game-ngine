# The editor's portable command layer -- undo/redo, the built-in commands and
# Play-in-Editor -- needs only the engine, so it is its own library: the
# editor UI links it, and so does the MCP server (mcp/), which runs headless.
add_library(aether_editor_core STATIC
    command_stack.cpp
    commands.cpp
    play_session.cpp
)
target_include_directories(aether_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(aether_editor_core PUBLIC Aether::Engine)
