if(WIN32)
    add_executable(aether_gltf_demo main.cpp)
    target_link_libraries(aether_gltf_demo PRIVATE Aether::Engine)
    target_include_directories(aether_gltf_demo PRIVATE ${stb_SOURCE_DIR})
    target_compile_definitions(aether_gltf_demo PRIVATE AETHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/")

    # See sandbox/CMakeLists.txt's comment: dxcompiler.dll is an implicit
    # load-time dependency of anything linking aether_engine.lib and calling
    # CompileHLSL, on WIN32.
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_gltf_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_gltf_demo>/dxcompiler.dll"
        )
    endif()
endif()
