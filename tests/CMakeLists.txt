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
    test_archive.cpp
    test_entity_guid.cpp
    test_hierarchy.cpp
    test_reflection_arrays.cpp
    test_project.cpp
    test_asset_database.cpp
    test_asset_refs.cpp
    test_importers.cpp
    test_model_importer.cpp
    test_hot_reload.cpp
    test_content_browser.cpp
    test_prefab.cpp
    test_prefab_library.cpp
    test_gameplay.cpp
    test_scheduler.cpp
    test_input.cpp
    test_input_bindings.cpp
    test_assets.cpp
    test_gltf_loader.cpp
)

target_link_libraries(aether_tests PRIVATE Aether::Engine)

if(TARGET aether_editor_ui)
    target_sources(aether_tests PRIVATE test_editor_ui.cpp test_command_stack.cpp test_commands.cpp test_play_session.cpp test_editor_projects.cpp test_script_inspector.cpp test_code_editor.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::EditorUI)
endif()
target_include_directories(aether_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Absolute path of the repo's sample assets, so tests can import real files
# regardless of the working directory they run from.
target_compile_definitions(aether_tests PRIVATE AETHER_REPO_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

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

if(TARGET Aether::Scripting)
    target_sources(aether_tests PRIVATE test_luau_host.cpp test_luau_bindings.cpp test_script_system.cpp test_script_api.cpp test_script_reload.cpp test_script_debugger.cpp test_script_dap.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Scripting)
endif()

target_sources(aether_tests PRIVATE test_blueprint_graph.cpp test_blueprint_vm.cpp test_blueprint_latent.cpp)
target_link_libraries(aether_tests PRIVATE Aether::Blueprint)

add_test(NAME aether_tests COMMAND aether_tests)
