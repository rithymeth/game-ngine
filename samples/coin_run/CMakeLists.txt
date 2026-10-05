# Coin Run: the sample game. The gameplay library is portable (and covered by
# tests/test_coin_run.cpp); the playable executable is Win32-only.
add_library(coin_run_game STATIC coin_run.cpp)
target_include_directories(coin_run_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(coin_run_game PUBLIC Aether::Engine)

if(WIN32)
    add_executable(coin_run main.cpp)
    target_link_libraries(coin_run PRIVATE coin_run_game)

    # Same load-time dependency as the sandbox: see sandbox/CMakeLists.txt.
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET coin_run POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:coin_run>/dxcompiler.dll"
        )
    endif()
endif()
