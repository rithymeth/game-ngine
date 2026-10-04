# Platform plugins (Phase 24, docs/design/PHASE_SPECS.md §24.5).
#
# A platform the engine doesn't build in (Android, a console) lives in its
# own directory with a CMakeLists.txt that calls aether_platform_plugin().
# List such directories in AETHER_PLATFORM_PLUGINS (a ;-separated CMake
# list, paths absolute or relative to the source tree), and every executable
# that calls aether_link_platform_plugins() gets them.
#
#   aether_platform_plugin(NAME android
#       SOURCES android_platform.cpp
#       LIBRARIES android log
#       DEFINITIONS AETHER_PLATFORM_ANDROID)
#
# Each plugin is an OBJECT library, so its AETHER_PLATFORM_PLUGIN
# registration is linked in even though nothing calls into it.

function(aether_platform_plugin)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBRARIES;DEFINITIONS;INCLUDES" ${ARGN})
    if(NOT ARG_NAME)
        message(FATAL_ERROR "aether_platform_plugin: NAME is required")
    endif()
    set(target aether_platform_${ARG_NAME})
    add_library(${target} OBJECT ${ARG_SOURCES})
    target_link_libraries(${target} PUBLIC Aether::Engine ${ARG_LIBRARIES})
    if(ARG_DEFINITIONS)
        target_compile_definitions(${target} PUBLIC ${ARG_DEFINITIONS})
    endif()
    if(ARG_INCLUDES)
        target_include_directories(${target} PUBLIC ${ARG_INCLUDES})
    endif()
    set_property(GLOBAL APPEND PROPERTY AETHER_PLATFORM_PLUGIN_TARGETS ${target})
    message(STATUS "Platform plugin: ${ARG_NAME}")
endfunction()

function(aether_link_platform_plugins target)
    get_property(plugins GLOBAL PROPERTY AETHER_PLATFORM_PLUGIN_TARGETS)
    foreach(plugin ${plugins})
        target_link_libraries(${target} PRIVATE ${plugin})
    endforeach()
endfunction()

function(aether_add_platform_plugins)
    foreach(dir ${AETHER_PLATFORM_PLUGINS})
        get_filename_component(abs "${dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        get_filename_component(name "${abs}" NAME)
        add_subdirectory("${abs}" "${CMAKE_BINARY_DIR}/platform_plugins/${name}")
    endforeach()
endfunction()
