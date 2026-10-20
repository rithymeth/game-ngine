# Beacon Run: a portable ECS gameplay library and a Win32 software-rendered game.
add_library(beacon_run_game STATIC beacon_run.cpp)
target_include_directories(beacon_run_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(beacon_run_game PUBLIC Aether::Engine)

if(WIN32)
    add_executable(beacon_run main.cpp)
    target_link_libraries(beacon_run PRIVATE beacon_run_game)

    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET beacon_run POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:beacon_run>/dxcompiler.dll"
        )
    endif()
endif()
