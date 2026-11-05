# aether_soak (Phase 47 step 5): repeated cycles of long-running work (entity churn, streaming a world in and out,
# replication with spawns and despawns) with a leak gate on live heap bytes. It replaces global operator new/delete
# for itself only, so it is a program of its own. AETHER_SOAK_SCALE=N runs N times the cycles (default 1).
add_executable(aether_soak main.cpp)
target_link_libraries(aether_soak PRIVATE Aether::Engine Aether::Streaming Aether::Net Aether::Audio)
add_test(NAME soak COMMAND aether_soak)
aether_test_scratch_dir(soak)
# A graphics-driver DLL is an implicit load-time dependency of anything that pulls in the renderer, so
# running this by hand from the build tree failed to start at all (STATUS_DLL_NOT_FOUND) while the
# ctest entry passed, because ctest only ran it once vcvars had put the DLLs on PATH. Copy them beside
# the executable so it runs on its own, the way the editor and the samples already do.
if(WIN32 AND AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_soak POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_soak>/dxcompiler.dll")
endif()
if(WIN32 AND AETHER_DXIL_DLL)
    add_custom_command(TARGET aether_soak POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_DXIL_DLL}" "$<TARGET_FILE_DIR:aether_soak>/dxil.dll")
endif()
