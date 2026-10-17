# Model Context Protocol server for the editor's scene (see mcp_server.h,
# editor_tools.h). Headless: depends only on the engine, the editor's portable
# command layer and the gameplay modules whose components it exposes -- not on
# ImGui or any graphics backend, and never on Aether::AI
# (docs/design/agent_gameplay_ai_boundary.md).
add_library(aether_mcp STATIC mcp_server.cpp permissions.cpp editor_tools.cpp modules.cpp simulation.cpp game_tools.cpp subprocess.cpp build_tools.cpp screenshot.cpp asset_tools.cpp kit_tools.cpp audio_tools.cpp vfx_tools.cpp terrain_tools.cpp nav_tools.cpp blueprint_tools.cpp loc_tools.cpp sequence_tools.cpp tilemap_tools.cpp script_tools.cpp animation_tools.cpp ui_tools.cpp save_tools.cpp streaming_tools.cpp material_tools.cpp prefab_tools.cpp)
target_include_directories(aether_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(aether_mcp PRIVATE /bigobj)
endif()
target_link_libraries(aether_mcp PUBLIC Aether::Engine aether_editor_core nlohmann_json::nlohmann_json)
# Modules whose components are exposed to agents.
target_link_libraries(aether_mcp PRIVATE
    Aether::Animation Aether::Audio Aether::Blueprint Aether::Gameplay Aether::Nav
    Aether::Renderer Aether::Sequencer Aether::Sprite2D Aether::Streaming Aether::Terrain Aether::UI Aether::VFX
    Aether::Game Aether::Loc)
foreach(kit Physics Inventory Interaction Quests)
    string(TOUPPER ${kit} kit_upper)
    if(TARGET Aether::${kit})
        target_link_libraries(aether_mcp PRIVATE Aether::${kit})
        target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_${kit_upper}=1)
    else()
        target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_${kit_upper}=0)
    endif()
endforeach()
# stb_image_write.h, for encoding the screenshots (the stb repo the engine already fetches).
target_include_directories(aether_mcp PRIVATE ${stb_SOURCE_DIR})
# Luau scripting tools, where the engine has the scripting module.
if(TARGET Aether::Scripting)
    target_link_libraries(aether_mcp PRIVATE Aether::Scripting)
    target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_SCRIPTING=1)
else()
    target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_SCRIPTING=0)
endif()
# Project templates, for project_create (they are made before this directory).
if(TARGET Aether::Templates)
    target_link_libraries(aether_mcp PRIVATE Aether::Templates)
    target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_TEMPLATES=1)
else()
    target_compile_definitions(aether_mcp PUBLIC AETHER_MCP_TEMPLATES=0)
endif()
add_library(Aether::MCP ALIAS aether_mcp)

add_executable(aether_mcp_server main.cpp)
target_link_libraries(aether_mcp_server PRIVATE aether_mcp)
if(WIN32 AND AETHER_DXCOMPILER_DLL)
    # Same load-time dependency as the sandbox: see sandbox/CMakeLists.txt.
    add_custom_command(TARGET aether_mcp_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_mcp_server>/dxcompiler.dll")
endif()

# The hosted Game starts the runtime modules its manifest names; link the
# plugin modules it can honour. Not "AI": the MCP side must never link gameplay
# AI (docs/design/agent_gameplay_ai_boundary.md). The plugin targets are made
# after this directory, hence the deferral.
function(aether_mcp_link_game_modules)
    foreach(module Audio Navigation Networking Physics)
        if(TARGET aether_module_${module})
            target_link_libraries(aether_mcp_server PRIVATE aether_module_${module})
        endif()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL aether_mcp_link_game_modules)
