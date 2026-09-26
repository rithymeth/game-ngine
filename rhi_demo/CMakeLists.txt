add_executable(aether_rhi_demo main.cpp)
target_link_libraries(aether_rhi_demo PRIVATE Aether::Engine)

if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_rhi_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_rhi_demo>/dxcompiler.dll"
    )
endif()
