add_executable(aether_tests
    main.cpp
    test_memory.cpp
    test_math.cpp
    test_filesystem.cpp
    test_job_system.cpp
    test_ecs.cpp
    test_serialization.cpp
    test_reflection.cpp
    test_reflection_any.cpp
    test_assets.cpp
    test_gltf_loader.cpp
)

target_link_libraries(aether_tests PRIVATE Aether::Engine)
target_include_directories(aether_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_sources(aether_tests PRIVATE test_render_graph.cpp test_rhi.cpp test_asset_manager.cpp test_material.cpp)

    # See sandbox/CMakeLists.txt's comment: dxcompiler.dll is an implicit
    # load-time dependency of anything linking aether_engine.lib whose link
    # pulls in a CompileHLSL/CompileHLSLToSPIRV reference — test_rhi.cpp
    # exercising the Vulkan backend's CreatePipeline (added for the RHI's
    # "Unified Cross-API Renderer" follow-up) is now one of those.
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_tests>/dxcompiler.dll"
        )
    endif()
endif()

if(TARGET Aether::Physics)
    target_sources(aether_tests PRIVATE test_physics.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Physics)
endif()

add_test(NAME aether_tests COMMAND aether_tests)
