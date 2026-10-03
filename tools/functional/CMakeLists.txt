# aether_functional (Phase 23 step 5): runs the functional tests registered
# with AETHER_FUNCTIONAL_TEST headless and writes JUnit XML or JSON for CI.
# Games link their own scenarios into a runner like this one; the samples
# here exercise the framework.
add_executable(aether_functional main.cpp sample_scenarios.cpp)
target_link_libraries(aether_functional PRIVATE Aether::Engine)
target_compile_features(aether_functional PRIVATE cxx_std_20)
if(AETHER_BUILD_TESTS)
    add_test(NAME aether_functional COMMAND aether_functional --junit ${CMAKE_BINARY_DIR}/functional-results.xml)
endif()
