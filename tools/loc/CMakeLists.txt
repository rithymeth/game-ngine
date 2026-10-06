# aether_loc: gathers a project's localizable strings into a string table and
# reads and writes .po files for translators (Phase 29 step 4).
add_executable(aether_loc_tool main.cpp)
set_target_properties(aether_loc_tool PROPERTIES OUTPUT_NAME aether_loc)
target_link_libraries(aether_loc_tool PRIVATE Aether::Engine Aether::Loc)
aether_link_platform_plugins(aether_loc_tool)
