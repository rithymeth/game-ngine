include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(aether_install_target target export_name)
    if(NOT TARGET "${target}")
        return()
    endif()

    cmake_parse_arguments(PKG "" "" "BUILD_INCLUDES;INSTALL_INCLUDES;HEADERS;HEADER_DESTINATIONS" ${ARGN})

    set_target_properties("${target}" PROPERTIES EXPORT_NAME "${export_name}")
    if(target STREQUAL "aether-json-dependency")
        # nlohmann is bundled into the SDK headers, so consumers must not need
        # the FetchContent target from the engine's build tree.
        set_property(TARGET "${target}" PROPERTY INTERFACE_LINK_LIBRARIES "")
        set_property(TARGET "${target}" PROPERTY INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>;$<INSTALL_INTERFACE:include>")
    endif()
    set(_interface_includes "")
    foreach(_include IN LISTS PKG_BUILD_INCLUDES)
        list(APPEND _interface_includes "$<BUILD_INTERFACE:${_include}>")
    endforeach()
    foreach(_include IN LISTS PKG_INSTALL_INCLUDES)
        list(APPEND _interface_includes "$<INSTALL_INTERFACE:${_include}>")
    endforeach()
    set_property(TARGET "${target}" PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_interface_includes}")
    set_property(TARGET "${target}" PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

    install(TARGETS "${target}" EXPORT AetherTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    if(PKG_HEADERS)
        list(LENGTH PKG_HEADERS _header_count)
        math(EXPR _last_header "${_header_count} - 1")
        foreach(_index RANGE 0 ${_last_header})
            list(GET PKG_HEADERS ${_index} _header_dir)
            if(PKG_HEADER_DESTINATIONS)
                list(GET PKG_HEADER_DESTINATIONS ${_index} _header_destination)
            else()
                set(_header_destination "${CMAKE_INSTALL_INCLUDEDIR}")
            endif()
            install(DIRECTORY "${_header_dir}/" DESTINATION "${_header_destination}"
                    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")
        endforeach()
    endif()
endfunction()

function(aether_generate_package_config)
    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/AetherConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/AetherConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Aether"
    )
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/AetherConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT AetherTargets
        FILE AetherTargets.cmake
        NAMESPACE Aether::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Aether"
    )
    install(FILES
        "${PROJECT_BINARY_DIR}/AetherConfig.cmake"
        "${PROJECT_BINARY_DIR}/AetherConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Aether"
    )
endfunction()
