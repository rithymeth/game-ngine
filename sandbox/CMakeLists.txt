if(WIN32)
    add_executable(aether_sandbox main.cpp)
    target_link_libraries(aether_sandbox PRIVATE Aether::Engine)
    # Absolute path to the repo's assets/ directory, baked in at configure
    # time — simplest thing that works for a demo run from the build tree
    # regardless of the current working directory; a shipped build would
    # instead resolve this relative to the executable (see the packaging/
    # build-system follow-up item).
    target_compile_definitions(aether_sandbox PRIVATE AETHER_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/")

    # aether_engine.lib's shader_compiler.cpp references DxcCreateInstance
    # (for CompileHLSLToSPIRV) unconditionally, so dxcompiler.dll is a
    # load-time dependency of every executable linking the engine — even
    # this one, which never actually calls that function. Same fix as
    # rhi_demo's CMakeLists (see its comment for why the SDK's own
    # dxcompiler.dll wouldn't be enough anyway).
    if(AETHER_DXCOMPILER_DLL)
        add_custom_command(TARGET aether_sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_sandbox>/dxcompiler.dll"
        )
    endif()
endif()
