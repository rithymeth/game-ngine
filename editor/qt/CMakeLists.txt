add_executable(aether_editor_qt WIN32 main.cpp editor_play_runtime.cpp physics_component_panel.cpp)
target_link_libraries(aether_editor_qt PRIVATE
    Aether::Engine
    Aether::Audio
    Aether::Gameplay
    Aether::Physics
    Aether::Sequencer
    aether_editor_core
    Qt6::Widgets
)
target_compile_features(aether_editor_qt PRIVATE cxx_std_17)
target_compile_definitions(aether_editor_qt PRIVATE
    AETHER_QT_TEST_MODEL_PATH="${CMAKE_SOURCE_DIR}/assets/models/test_textured_cube.gltf"
)
set_target_properties(aether_editor_qt PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME aether_editor
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/editor"
)

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
                "$<TARGET_FILE:aether_editor_qt>"
        VERBATIM
        COMMENT "Deploying the Qt runtime for the default editor executable"
    )

    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_editor_qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_editor_qt>/dxcompiler.dll"
            VERBATIM
        )
    endif()
    if(AETHER_DXIL_DLL)
        add_custom_command(TARGET aether_editor_qt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXIL_DLL}" "$<TARGET_FILE_DIR:aether_editor_qt>/dxil.dll"
            VERBATIM
        )
    endif()
endif()

if(AETHER_BUILD_SCRIPTING)
    target_sources(aether_editor_qt PRIVATE script_component_editor.cpp)
    target_link_libraries(aether_editor_qt PRIVATE Aether::Scripting)
    target_compile_definitions(aether_editor_qt PRIVATE AETHER_QT_EDITOR_SCRIPTING=1)
endif()

if(MSVC)
    target_compile_options(aether_editor_qt PRIVATE /W4 /permissive-)
endif()
