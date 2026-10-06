# Model Context Protocol server for the editor's scene (see mcp_server.h,
# editor_tools.h). Headless: depends only on the engine and the editor's
# portable command layer, not on ImGui or any graphics backend.
add_library(aether_mcp STATIC mcp_server.cpp editor_tools.cpp)
target_include_directories(aether_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aether_mcp PUBLIC Aether::Engine aether_editor_core nlohmann_json::nlohmann_json)
add_library(Aether::MCP ALIAS aether_mcp)

add_executable(aether_mcp_server main.cpp)
target_link_libraries(aether_mcp_server PRIVATE aether_mcp)
if(WIN32 AND AETHER_DXCOMPILER_DLL)
    # Same load-time dependency as the sandbox: see sandbox/CMakeLists.txt.
    add_custom_command(TARGET aether_mcp_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_mcp_server>/dxcompiler.dll")
endif()
