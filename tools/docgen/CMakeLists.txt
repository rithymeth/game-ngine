# aether_docgen: writes the API reference from the reflection registry
# (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.7).
add_executable(aether_docgen main.cpp)
target_link_libraries(aether_docgen PRIVATE Aether::Engine Aether::Blueprint Aether::Renderer Aether::Animation Aether::Audio Aether::UI Aether::VFX Aether::AI Aether::Nav Aether::Net Aether::Streaming Aether::Sprite2D)
if(TARGET Aether::Physics)
    target_link_libraries(aether_docgen PRIVATE Aether::Physics)
    target_compile_definitions(aether_docgen PRIVATE AETHER_DOCGEN_PHYSICS=1)
endif()

# See sandbox/CMakeLists.txt: dxcompiler.dll is a load-time dependency of
# anything linking aether_engine on Windows.
if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_docgen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_docgen>/dxcompiler.dll"
    )
endif()
