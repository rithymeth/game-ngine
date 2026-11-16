# Abyss Protocol: underwater exploration sample for Aether.
# Gameplay library is portable; playable executable is Win32-only for now (mirrors gem_hop).
add_library(abyss_protocol_game STATIC abyss_protocol.cpp)
target_include_directories(abyss_protocol_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(abyss_protocol_game PUBLIC Aether::Engine)

if(WIN32)
    add_executable(abyss_protocol main.cpp)
    target_link_libraries(abyss_protocol PRIVATE abyss_protocol_game)
    target_compile_definitions(abyss_protocol PRIVATE ABYSS_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET abyss_protocol POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:abyss_protocol>/dxcompiler.dll"
        )
    endif()
endif()
