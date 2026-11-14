add_executable(aether_tests
    main.cpp
    test_memory.cpp
    test_math.cpp
    test_filesystem.cpp
    test_job_system.cpp
    test_platform_lifecycle.cpp
    test_meshlet_cook.cpp test_mesh_cook.cpp test_mesh_summary.cpp test_format_versions.cpp test_ai_decision.cpp test_ai_utility_loader.cpp test_alloc_track.cpp test_test_world.cpp
    test_kit_registry.cpp
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
    test_model_importer.cpp test_import_diagnostics.cpp
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
    test_skinning.cpp
)

target_link_libraries(aether_tests PRIVATE Aether::Engine)
if(WIN32)
    # Package_ThePackagedGameCarriesItsOwnVersionInformation reads a staged exe's
    # version resource back with the Windows version APIs.
    target_link_libraries(aether_tests PRIVATE version)
endif()

if(TARGET coin_run_game)
    target_sources(aether_tests PRIVATE test_coin_run.cpp)
    target_link_libraries(aether_tests PRIVATE coin_run_game)
endif()

if(TARGET gem_hop_game)
    target_sources(aether_tests PRIVATE test_gem_hop.cpp)
    target_link_libraries(aether_tests PRIVATE gem_hop_game)
endif()

if(TARGET aether_mcp)
    target_sources(aether_tests PRIVATE test_mcp.cpp test_legacy_roundtrip.cpp)
    target_link_libraries(aether_tests PRIVATE aether_mcp)
endif()
# Phase 25 step 4: the player's runtime.
target_sources(aether_tests PRIVATE test_player.cpp test_player_lights.cpp test_player_particles.cpp test_player_save.cpp test_loc_runtime.cpp test_package.cpp test_pak_patch.cpp test_plugin.cpp test_templates.cpp test_aether01.cpp test_skinned_instances.cpp test_player_backend_parity.cpp test_procedural_player.cpp)
target_link_libraries(aether_tests PRIVATE Aether::Game Aether::Templates Aether::Blueprint Aether::ServerCore)
if(TARGET Aether::Physics)
    target_link_libraries(aether_tests PRIVATE Aether::Physics)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_PHYSICS=1)
else()
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_PHYSICS=0)
endif()
# The Build and Package window's test packages and launches the real player.
add_dependencies(aether_tests aether_player)
target_compile_definitions(aether_tests PRIVATE AETHER_TEST_PLAYER_PATH="$<TARGET_FILE:aether_player>")
aether_link_platform_plugins(aether_tests)

if(TARGET aether_editor_ui)
    target_sources(aether_tests PRIVATE test_editor_ui.cpp test_command_stack.cpp test_commands.cpp test_play_session.cpp test_editor_projects.cpp test_script_inspector.cpp test_code_editor.cpp test_graph_editor.cpp test_blueprint_editor.cpp test_material_editor.cpp test_anim_editors.cpp test_audio_editors.cpp test_ui_designer.cpp test_vfx_editor.cpp test_ai_editor.cpp test_world_editor.cpp test_procedural_editor.cpp test_scene_panel.cpp test_scene_preview.cpp test_format_fixtures.cpp test_net_editor.cpp test_console_panel.cpp test_profiler_panel.cpp test_debug_overlay.cpp test_crash_reporter.cpp test_editor_host.cpp test_editor_workspace.cpp test_editor_commands.cpp test_notifications.cpp test_file_dialog.cpp test_project_hub.cpp test_main_toolbar.cpp test_build_window.cpp test_extensions.cpp test_tilemap_editor.cpp test_sequencer_editor.cpp test_save_inspector.cpp test_loc_editor.cpp test_gameplay_debugger.cpp test_vcs.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::EditorUI)
    if(TARGET aether_editor_vulkan)
        target_link_libraries(aether_tests PRIVATE Aether::EditorVulkan)
        target_compile_definitions(aether_tests PRIVATE AETHER_HAS_EDITOR_VULKAN)
    endif()
endif()
if(TARGET aether_editor_core)
    target_sources(aether_tests PRIVATE test_scene_document.cpp test_scene_crash_recovery.cpp test_editor_settings.cpp test_scene_autosave.cpp test_editor_document.cpp)
    target_link_libraries(aether_tests PRIVATE aether_editor_core)
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
    # dxil.dll signs the DXIL that CompileHLSLToDXIL produces; D3D12 rejects unsigned shaders.
    if(AETHER_DXIL_DLL)
        add_custom_command(TARGET aether_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXIL_DLL}" "$<TARGET_FILE_DIR:aether_tests>/dxil.dll"
        )
    endif()
endif()

if(TARGET Aether::Physics)
    target_sources(aether_tests PRIVATE test_physics.cpp test_physics_scene.cpp test_physics_events.cpp test_physics_queries.cpp test_character.cpp test_physics_debug.cpp test_physics_constraints.cpp test_animation_root_motion.cpp test_procedural_walk.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Physics)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_PLAYER_PHYSICS=1)
endif()
if(TARGET Aether::PhysicsNet)
    target_sources(aether_tests PRIVATE test_physics_net.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::PhysicsNet)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_PHYSICS_NET=1)
endif()

if(TARGET Aether::Scripting)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_SCRIPTING=1)
    target_sources(aether_tests PRIVATE test_gameplay_script.cpp test_luau_host.cpp test_luau_bindings.cpp test_script_system.cpp test_script_api.cpp test_script_reload.cpp test_script_debugger.cpp test_script_dap.cpp test_nav_script.cpp test_ai_script.cpp test_ai_nav_api.cpp test_net_rpc_script.cpp test_debug_draw_script.cpp test_api_inventory.cpp test_compat_corpus.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Scripting)
endif()

target_sources(aether_tests PRIVATE test_blueprint_graph.cpp test_blueprint_vm.cpp test_blueprint_latent.cpp test_blueprint_nodes.cpp test_blueprint_arrays.cpp test_blueprint_world.cpp test_blueprint_messages.cpp test_blueprint_macros.cpp test_blueprint_sort.cpp test_blueprint_debugger.cpp test_blueprint_samples.cpp test_renderer.cpp test_renderer_lighting.cpp test_frame_graph.cpp test_post.cpp test_material_graph.cpp test_material_codegen.cpp test_material_instance.cpp test_material_functions.cpp test_animation.cpp test_animation_blending.cpp test_anim_graph.cpp test_animator.cpp test_ik.cpp test_kael_character.cpp test_audio.cpp test_audio_3d.cpp test_audio_cues.cpp test_audio_system.cpp test_audio_threading.cpp test_ui_layout.cpp test_ui_input.cpp test_ui_assets.cpp test_ui_widgets.cpp test_ui_text.cpp test_ui_fallback.cpp test_ui_loc.cpp test_vfx_particles.cpp test_vfx_render.cpp test_vfx_events.cpp test_vfx_components.cpp test_vfx_gpu.cpp test_nav_mesh.cpp test_nav_dynamic.cpp test_nav_crowd.cpp test_legacy_files.cpp test_nav_hierarchical.cpp test_streaming_flythrough.cpp test_ai_behavior_tree.cpp
    test_terrain.cpp
    test_terrain_splat.cpp
    test_terrain_renderer.cpp
    test_terrain_foliage.cpp
    test_terrain_generator.cpp
    test_scatter.cpp
    test_procedural_mesh.cpp
    test_terrain_spline.cpp test_terrain_gpu.cpp test_ai_perception.cpp test_streaming.cpp test_sprite2d.cpp test_sequence_core.cpp test_sequence_system.cpp test_cine_movie.cpp test_sequence_fade_sub.cpp test_sequence_subtitle.cpp test_sequence_vfx.cpp test_save.cpp test_world_state.cpp test_settings.cpp test_save_games.cpp test_loc.cpp test_loc_tools.cpp test_gameplay_tags.cpp test_gameplay_attributes.cpp test_gameplay_effects.cpp test_gameplay_abilities.cpp test_api_docs.cpp test_loader_robustness.cpp test_api_stability.cpp test_manual.cpp test_net_transport.cpp test_net_replication.cpp test_net_rpc.cpp test_net_prediction.cpp test_net_session.cpp test_net_production.cpp test_dedicated_server.cpp test_console.cpp test_profiler.cpp test_debug_draw.cpp test_crash.cpp test_functional.cpp test_window.cpp test_vulkan_rhi.cpp test_rhi_lifetime.cpp test_gfx_parity.cpp test_lit_parity.cpp test_scene_shader.cpp test_decal.cpp test_post_pass.cpp test_gltf_glb.cpp test_bloom.cpp test_bindless_table.cpp test_shadows_pass.cpp test_ibl.cpp test_material_loading.cpp test_light_clusters.cpp test_taa.cpp test_readback.cpp test_ray_tracing_probe.cpp test_gpu_timing.cpp test_virtual_shadow.cpp test_vsm_pages.cpp test_vsm_scene.cpp test_water_animation.cpp test_wind.cpp test_material_overrides.cpp test_camera_path.cpp test_view_cull.cpp test_mesh_lod.cpp test_player_record.cpp test_platform_plugin.cpp test_pak.cpp test_cook.cpp test_texture_cook.cpp)
target_link_libraries(aether_tests PRIVATE Aether::Blueprint Aether::Renderer Aether::Animation Aether::Audio Aether::UI Aether::VFX Aether::Nav Aether::AI Aether::Terrain Aether::Streaming Aether::Sprite2D Aether::Sequencer Aether::Save Aether::Loc Aether::Gameplay Aether::Net)
if(TARGET aether_bench_lib)
    target_sources(aether_tests PRIVATE test_bench.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Bench)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_BENCH=1)
endif()
if(TARGET aether_quests)
    target_sources(aether_tests PRIVATE test_quests.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Quests)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_QUESTS=1)
endif()
if(TARGET aether_interaction)
    target_sources(aether_tests PRIVATE test_interaction.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Interaction)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_INTERACTION=1)
endif()
if(TARGET aether_inventory)
    target_sources(aether_tests PRIVATE test_inventory.cpp)
    target_link_libraries(aether_tests PRIVATE Aether::Inventory)
    target_compile_definitions(aether_tests PRIVATE AETHER_TEST_HAS_INVENTORY=1)
endif()

add_test(NAME aether_tests COMMAND aether_tests)
aether_test_scratch_dir(aether_tests)
# The module-graph check must catch a cycle (Phase 37 step 2).
if(Python3_Interpreter_FOUND)
    add_test(NAME module_graph_catches_a_cycle
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_module_graph.py --dot ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/module_graph_cycle.dot --no-includes)
    set_tests_properties(module_graph_catches_a_cycle PROPERTIES WILL_FAIL TRUE)
    add_test(NAME api_stability_annotations COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_api_stability.py)
    # The game's own asset check (buffer lengths against what each glTF declares, scene references, unique GUIDs). It was written and
    # never run by anything, and the valley's buffer sat truncated in the repository: the environment loaded as nothing.
    add_test(NAME aether01_assets_validate COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/games/AETHER-01/tools/validate_assets.py)
    # Two MCP files registering one tool name is neither a compile error nor a test failure -- the tools
    # live in separate translation units, so only the far end of the server ever sees both.
    add_test(NAME unlisted_sources COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_sources_listed.py)
    add_test(NAME unlisted_sources_catch_a_miss
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_sources_listed.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unlisted_source)
    set_tests_properties(unlisted_sources_catch_a_miss PROPERTIES WILL_FAIL TRUE)
    add_test(NAME mcp_tool_names COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_mcp_tool_names.py)
    add_test(NAME mcp_tool_names_catch_a_duplicate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_mcp_tool_names.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mcp_tools_duplicate)
    set_tests_properties(mcp_tool_names_catch_a_duplicate PROPERTIES WILL_FAIL TRUE)
    add_test(NAME api_freeze_checks COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_api_freeze.py)
    add_test(NAME api_inventory COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/api_inventory.py)
    # Phase 48 step 2: the serialized-format table has to keep pointing at the files that implement
    # each format. A rename otherwise leaves it documenting something that is no longer there.
    add_test(NAME format_versions_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_format_versions.py)
    # The root README promises docs/README.md lists every doc. It listed 17 of 53 before this check.
    add_test(NAME docs_index COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_docs_index.py)
    # The index says a document exists; this says the documents' own links resolve. A game checklist called a
    # design document its canonical reference for weeks while the file was never in the repository.
    add_test(NAME doc_links COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_doc_links.py)
    add_test(NAME production_gate_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gate.py)
    add_test(NAME m8_gate_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gate.py
        --table ${CMAKE_SOURCE_DIR}/docs/design/m8_gates.json)
    # M8.10: release manifest, install, patch, rollback and build-id checks of tools/ship/aether_ship.py.
    add_test(NAME ship_tool_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ship/test_aether_ship.py)
endif()
