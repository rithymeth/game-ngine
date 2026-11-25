# The actual game is a separate worktree, not a dependency to fetch. Test
# this only from an engine-only tree; a real checkout may arrive since configure.
if(EXISTS "${AETHER_SOURCE_DIR}/games/EVERVALE-REBOOT/CMakeLists.txt")
    message("SKIP: this workspace now has the Evervale checkout")
    return()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${AETHER_SOURCE_DIR}" -B "${AETHER_CHECK_DIR}"
        -DAETHER_BUILD_EVERVALE_REBOOT=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 45
)
if("${status}" STREQUAL "0" OR
   NOT "${output}\n${errors}" MATCHES "AETHER_BUILD_EVERVALE_REBOOT=ON requires the Evervale source checkout" OR
   NOT "${output}\n${errors}" MATCHES "games/EVERVALE-REBOOT/CMakeLists.txt")
    message(FATAL_ERROR "Expected a missing Evervale checkout error, got ${status}:\n${output}\n${errors}")
endif()
