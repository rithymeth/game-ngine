add_executable(aether_editor main.cpp)
target_link_libraries(aether_editor PRIVATE Aether::Engine Aether::Physics Aether::EditorUI Aether::DevTools ImGui::ImGui)
if(NOT AETHER_BUILD_EDITOR_UI)
    message(FATAL_ERROR "The editor needs AETHER_BUILD_EDITOR_UI=ON (its generic Inspector lives there)")
endif()
# For the glTF integration follow-up: loads assets/models/test_animation.gltf
# at a real filesystem path, same convention gltf_demo already uses, and
# dumps a screenshot via stb_image_write (same as gltf_demo/pbr_demo).
target_compile_definitions(aether_editor PRIVATE AETHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/")
target_include_directories(aether_editor PRIVATE ${stb_SOURCE_DIR})

# See sandbox/CMakeLists.txt's comment: dxcompiler.dll is an implicit
# load-time dependency of anything linking aether_engine.lib on WIN32.
if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_editor>/dxcompiler.dll"
    )
endif()
