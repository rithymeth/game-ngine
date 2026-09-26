add_executable(aether_editor main.cpp)
target_link_libraries(aether_editor PRIVATE Aether::Engine Aether::Physics ImGui::ImGui)

# See sandbox/CMakeLists.txt's comment: dxcompiler.dll is an implicit
# load-time dependency of anything linking aether_engine.lib on WIN32.
if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_editor>/dxcompiler.dll"
    )
endif()
