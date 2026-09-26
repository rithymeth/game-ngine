if(WIN32)
    add_executable(aether_pbr_demo main.cpp)
    target_link_libraries(aether_pbr_demo PRIVATE Aether::Engine)
    # stb_image_write.h, for the optional screenshot-to-PNG capture (same
    # stb repo already fetched for the engine's asset pipeline).
    target_include_directories(aether_pbr_demo PRIVATE ${stb_SOURCE_DIR})

    # See sandbox/CMakeLists.txt's comment: dxcompiler.dll is an implicit
    # load-time dependency of anything linking aether_engine.lib and calling
    # CompileHLSL, on WIN32.
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_pbr_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_pbr_demo>/dxcompiler.dll"
        )
    endif()
endif()
