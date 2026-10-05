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
# Phase 25 step 4: the player's runtime.
target_sources(aether_tests PRIVATE test_player.cpp test_player_save.cpp test_loc_runtime.cpp test_package.cpp test_pak_patch.cpp test_plugin.cpp test_templates.cpp)
target_link_libraries(aether_tests PRIVATE Aether::Game Aether::Templates Aether::Blueprint)
# The Build and Package window's test packages and launches the real player.
add_dependencies(aether_tests aether_player)
target_compile_definitions(aether_tests PRIVATE AETHER_TEST_PLAYER_PATH="$<TARGET_FILE:aether_player>")
aether_link_platform_plugins(aether_tests)

if(TARGET aether_editor_ui)
    target_sources(aether_tests PRIVATE test_editor_ui.cpp test_command_stack.cpp test_commands.cpp test_play_session.cpp test_editor_projects.cpp test_script_inspector.cpp test_code_editor.cpp test_graph_editor.cpp test_blueprint_editor.cpp test_material_editor.cpp test_anim_editors.cpp test_audio_editors.cpp test_ui_designer.cpp test_vfx_editor.cpp test_ai_editor.cpp test_world_editor.cpp test_net_editor.cpp test_console_panel.cpp test_profiler_panel.cpp test_debug_overlay.cpp test_crash_reporter.cpp test_editor_host.cpp test_editor_workspace.cpp test_build_window.cpp test_extensions.cpp test_tilemap_editor.cpp test_sequencer_editor.cpp test_save_inspector.cpp test_loc_editor.cpp test_vcs.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::EditorUI)
    if(TARGET aether_editor_vulkan)
        target_link_libraries(aether_tests PRIVATE Aether::EditorVulkan)
        target_compile_definitions(aether_tests PRIVATE AETHER_HAS_EDITOR_VULKAN)
    endif()
endif()
target_include_directories(aether_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Absolute path of the repo's sample assets, so tests can import real files
# regardless of the working directory they run from.
target_compile_definitions(aether_tests PRIVATE AETHER_REPO_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets" AETHER_REPO_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")

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
    target_sources(aether_tests PRIVATE test_physics.cpp test_physics_scene.cpp test_physics_events.cpp test_physics_queries.cpp test_character.cpp test_physics_debug.cpp test_animation_root_motion.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Physics)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_PLAYER_PHYSICS=1)
endif()

if(TARGET Aether::Scripting)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_SCRIPTING=1)
    target_sources(aether_tests PRIVATE test_luau_host.cpp test_luau_bindings.cpp test_script_system.cpp test_script_api.cpp test_script_reload.cpp test_script_debugger.cpp test_script_dap.cpp test_nav_script.cpp test_ai_script.cpp test_net_rpc_script.cpp test_debug_draw_script.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Scripting)
endif()

target_sources(aether_tests PRIVATE test_blueprint_graph.cpp test_blueprint_vm.cpp test_blueprint_latent.cpp test_blueprint_nodes.cpp test_blueprint_arrays.cpp test_blueprint_world.cpp test_blueprint_messages.cpp test_blueprint_macros.cpp test_blueprint_sort.cpp test_blueprint_debugger.cpp test_blueprint_samples.cpp test_renderer.cpp test_renderer_lighting.cpp test_frame_graph.cpp test_post.cpp test_material_graph.cpp test_material_codegen.cpp test_material_instance.cpp test_material_functions.cpp test_animation.cpp test_animation_blending.cpp test_anim_graph.cpp test_animator.cpp test_ik.cpp test_audio.cpp test_audio_3d.cpp test_audio_cues.cpp test_audio_system.cpp test_audio_threading.cpp test_ui_layout.cpp test_ui_input.cpp test_ui_assets.cpp test_ui_widgets.cpp test_ui_text.cpp test_ui_fallback.cpp test_ui_loc.cpp test_vfx_particles.cpp test_vfx_render.cpp test_vfx_events.cpp test_vfx_components.cpp test_vfx_gpu.cpp test_nav_mesh.cpp test_nav_dynamic.cpp test_nav_crowd.cpp test_ai_behavior_tree.cpp
    test_terrain.cpp
    test_terrain_splat.cpp
    test_terrain_renderer.cpp
    test_terrain_foliage.cpp
    test_terrain_spline.cpp test_ai_perception.cpp test_streaming.cpp test_sprite2d.cpp test_sequence_core.cpp test_sequence_system.cpp test_cine_movie.cpp test_sequence_fade_sub.cpp test_save.cpp test_world_state.cpp test_settings.cpp test_save_games.cpp test_loc.cpp test_loc_tools.cpp test_gameplay_tags.cpp test_api_docs.cpp test_manual.cpp test_net_transport.cpp test_net_replication.cpp test_net_rpc.cpp test_net_prediction.cpp test_net_session.cpp test_console.cpp test_profiler.cpp test_debug_draw.cpp test_crash.cpp test_functional.cpp test_window.cpp test_vulkan_rhi.cpp test_platform_plugin.cpp test_pak.cpp test_cook.cpp test_texture_cook.cpp)
target_link_libraries(aether_tests PRIVATE Aether::Blueprint Aether::Renderer Aether::Animation Aether::Audio Aether::UI Aether::VFX Aether::Nav Aether::AI Aether::Terrain Aether::Streaming Aether::Sprite2D Aether::Sequencer Aether::Save Aether::Loc Aether::Gameplay Aether::Net)

add_test(NAME aether_tests COMMAND aether_tests)
