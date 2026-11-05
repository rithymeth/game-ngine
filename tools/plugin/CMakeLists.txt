# aether_plugin (Phase 46 step 3): package, inspect and install .apkg plugin packages
# from the command line. The packaging itself is engine/plugin/package.h.
add_executable(aether_plugin main.cpp)
target_link_libraries(aether_plugin PRIVATE Aether::Engine)

# The usage path refuses a missing command (exit code 2): a cheap smoke test that
# the tool runs at all. The packaging API is covered by test_plugin.
add_test(NAME plugin_package_cli COMMAND aether_plugin)
set_tests_properties(plugin_package_cli PROPERTIES WILL_FAIL TRUE)
aether_test_scratch_dir(plugin_package_cli)
