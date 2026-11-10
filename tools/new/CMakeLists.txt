# aether_new: starts a project from a template on the command line, the editor's New Project without the editor.
add_executable(aether_new main.cpp)
target_link_libraries(aether_new PRIVATE Aether::Templates)
aether_link_platform_plugins(aether_new)
# The tool runs and knows the templates: listing them names one. Creating a project is covered by test_templates.
if(AETHER_BUILD_TESTS)
    add_test(NAME aether_new_lists_templates COMMAND aether_new --list)
    set_tests_properties(aether_new_lists_templates PROPERTIES PASS_REGULAR_EXPRESSION "third_person")
endif()
