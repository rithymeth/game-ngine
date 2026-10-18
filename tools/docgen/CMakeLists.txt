# aether_docgen: writes the API reference from the reflection registry
# (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.7).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/site.js" AETHER_DOCGEN_SITE_JS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/site.css" AETHER_DOCGEN_SITE_CSS)
configure_file(site_assets.h.in "${CMAKE_CURRENT_BINARY_DIR}/site_assets.h" @ONLY)

add_executable(aether_docgen main.cpp site.cpp)
target_include_directories(aether_docgen PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(aether_docgen PRIVATE Aether::Engine Aether::Blueprint Aether::Renderer Aether::Animation Aether::Audio Aether::UI Aether::VFX Aether::AI Aether::Nav Aether::Net Aether::Streaming Aether::Sprite2D)
if(TARGET Aether::Physics)
    target_link_libraries(aether_docgen PRIVATE Aether::Physics)
    target_compile_definitions(aether_docgen PRIVATE AETHER_DOCGEN_PHYSICS=1)
endif()

add_custom_target(aether_docs_site
    COMMAND $<TARGET_FILE:aether_docgen>
            --site "${CMAKE_BINARY_DIR}/docs-site"
            --docs "${CMAKE_SOURCE_DIR}/docs"
    DEPENDS aether_docgen
    COMMENT "Generating the offline Aether documentation site"
    VERBATIM
)

# See sandbox/CMakeLists.txt: dxcompiler.dll is a load-time dependency of
# anything linking aether_engine on Windows.
if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_docgen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_docgen>/dxcompiler.dll"
    )
endif()
