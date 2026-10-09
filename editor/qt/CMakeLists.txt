add_executable(aether_editor_qt WIN32 main.cpp)
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
set_target_properties(aether_editor_qt PROPERTIES AUTOMOC ON)

if(MSVC)
    target_compile_options(aether_editor_qt PRIVATE /W4 /permissive-)
endif()
