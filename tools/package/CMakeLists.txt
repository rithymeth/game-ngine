# aether_package: cooks a project and stages it with the player as a runnable
# folder (Phase 25 step 5). The editor's Build and Package window calls the
# same engine function; this is that path on the command line.
add_executable(aether_package main.cpp)
target_link_libraries(aether_package PRIVATE Aether::Engine)
aether_link_platform_plugins(aether_package)
