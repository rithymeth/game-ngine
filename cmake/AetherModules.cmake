# Plugin modules (Phase 26 step 1, docs/design/PHASE_SPECS.md §26.1).
#
# A plugin's module is code that registers itself with AETHER_MODULE. Built
# with aether_module(), it's an OBJECT library, so the registration is
# linked in even though nothing calls into it; executables that host
# plugins (the player, the editors, the tests) call aether_link_modules().
#
#   aether_module(NAME Physics SOURCES physics_module.cpp LIBRARIES Aether::Physics)

function(aether_module)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBRARIES" ${ARGN})
    if(NOT ARG_NAME)
        message(FATAL_ERROR "aether_module: NAME is required")
    endif()
    set(target aether_module_${ARG_NAME})
    add_library(${target} OBJECT ${ARG_SOURCES})
    target_link_libraries(${target} PUBLIC Aether::Engine ${ARG_LIBRARIES})
    set_property(GLOBAL APPEND PROPERTY AETHER_MODULE_TARGETS ${target})
endfunction()

function(aether_link_modules target)
    get_property(modules GLOBAL PROPERTY AETHER_MODULE_TARGETS)
    foreach(module ${modules})
        target_link_libraries(${target} PRIVATE ${module})
    endforeach()
endfunction()
