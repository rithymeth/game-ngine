add_executable(aether_editor_qt WIN32 main.cpp editor_play_runtime.cpp physics_component_panel.cpp behavior_tree_editor_widget.cpp navigation_editor_widget.cpp
    animation_editor_widget.cpp anim_graph_editor_widget.cpp blend_space_editor_widget.cpp montage_editor_widget.cpp sound_cue_editor_widget.cpp blueprint_editor_widget.cpp material_editor_widget.cpp vfx_editor_widget.cpp
    ../src/graph/blueprint_graph_ops.cpp ../src/graph/blueprint_document.cpp
    ../src/graph/material_document.cpp ../src/audio/cue_document.cpp ../src/vfx/vfx_document.cpp ../src/vfx/vfx_preview.cpp ../src/ai/bt_document.cpp
    ../src/anim/anim_document.cpp ../src/anim/blend_space_document.cpp)
target_link_libraries(aether_editor_qt PRIVATE
    Aether::Engine
    Aether::Renderer
    Aether::Audio
    Aether::Animation
    Aether::Gameplay
    Aether::Physics
    Aether::AI
    Aether::Sequencer
    Aether::Blueprint
    Aether::VFX
    aether_editor_core
    Qt6::Widgets
)

# Keep the editor's component schema set aligned with the game's selected kits.
if(TARGET Aether::Inventory)
    target_link_libraries(aether_editor_qt PRIVATE Aether::Inventory)
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_INVENTORY=1)
else()
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_INVENTORY=0)
endif()
if(TARGET Aether::Interaction)
    target_link_libraries(aether_editor_qt PRIVATE Aether::Interaction)
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_INTERACTION=1)
else()
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_INTERACTION=0)
endif()
if(TARGET Aether::Quests)
    target_link_libraries(aether_editor_qt PRIVATE Aether::Quests)
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_QUESTS=1)
else()
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_KIT_QUESTS=0)
endif()
target_compile_features(aether_editor_qt PRIVATE cxx_std_17)
target_compile_definitions(aether_editor_qt PRIVATE
    AETHER_QT_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/assets/models/test_textured_cube.gltf"
)
set_target_properties(aether_editor_qt PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME aether_editor
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/editor"
)

if(CMAKE_CONFIGURATION_TYPES)
    set(_aether_qt_editor_runtime_dir "${CMAKE_BINARY_DIR}/editor/$<CONFIG>")
else()
    set(_aether_qt_editor_runtime_dir "${CMAKE_BINARY_DIR}/editor")
endif()

# The Qt editor is the default Windows launch target. Deploy its Qt runtime
# beside the executable in the build tree as well as in release packages, so
# opening build/editor/aether_editor.exe doesn't fall back to an older editor
# or fail because Qt's DLLs/plugins are missing.
if(WIN32)
    get_filename_component(_aether_qt_root "${Qt6Core_DIR}/../../.." ABSOLUTE)
    find_program(AETHER_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt.exe windeployqt
        HINTS "${_aether_qt_root}/bin"
        REQUIRED
    )
    add_custom_command(TARGET aether_editor_qt POST_BUILD
        COMMAND "${AETHER_WINDEPLOYQT_EXECUTABLE}" --release --no-translations
                "${_aether_qt_editor_runtime_dir}/aether_editor.exe"
        VERBATIM
        COMMENT "Deploying the Qt runtime for the default editor executable"
    )

    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_editor_qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "${_aether_qt_editor_runtime_dir}/dxcompiler.dll"
            VERBATIM
        )
    endif()
    if(AETHER_DXIL_DLL)
        add_custom_command(TARGET aether_editor_qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXIL_DLL}" "${_aether_qt_editor_runtime_dir}/dxil.dll"
            VERBATIM
        )
    endif()
endif()

if(AETHER_BUILD_SCRIPTING)
    target_sources(aether_editor_qt PRIVATE script_component_editor.cpp script_editor_widget.cpp)
    target_link_libraries(aether_editor_qt PRIVATE Aether::Scripting)
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_EDITOR_SCRIPTING=1)
endif()

if(MSVC)
    target_compile_options(aether_editor_qt PRIVATE /W4 /permissive-)
endif()
