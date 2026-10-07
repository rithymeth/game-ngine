# Project templates (Phase 26 step 2, docs/design/PHASE_SPECS.md §26.2):
# New Project from Blank, First Person, Third Person, Top Down or Vehicle.
add_library(aether_templates STATIC
    src/templates.cpp
    src/template_scripts.cpp
)
target_include_directories(aether_templates PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_templates PUBLIC Aether::Engine Aether::Blueprint Aether::Sprite2D)
if(TARGET Aether::Physics)
    # The Third Person template can use the existing character controller and
    # collider data when the engine build includes Jolt. Physics-disabled
    # builds keep the script-driven fallback.
    target_link_libraries(aether_templates PUBLIC Aether::Physics)
    target_compile_definitions(aether_templates PRIVATE AETHER_TEMPLATE_HAS_PHYSICS=1)
else()
    target_compile_definitions(aether_templates PRIVATE AETHER_TEMPLATE_HAS_PHYSICS=0)
endif()
add_library(Aether::Templates ALIAS aether_templates)
