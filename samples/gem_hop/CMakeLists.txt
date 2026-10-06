# Gem Hop: a 3D sample game. The gameplay library is portable (and covered by
# tests/test_gem_hop.cpp); the playable executable is Win32-only.
add_library(gem_hop_game STATIC gem_hop.cpp)
target_include_directories(gem_hop_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gem_hop_game PUBLIC Aether::Engine)

if(WIN32)
    add_executable(gem_hop main.cpp)
    target_link_libraries(gem_hop PRIVATE gem_hop_game)

    # Same load-time dependency as the sandbox: see sandbox/CMakeLists.txt.
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET gem_hop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:gem_hop>/dxcompiler.dll"
        )
    endif()
endif()
