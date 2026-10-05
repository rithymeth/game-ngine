# aether_cook: cooks a project into an .apak archive (Phase 25).
add_executable(aether_cook main.cpp)
target_link_libraries(aether_cook PRIVATE Aether::Engine)
aether_link_platform_plugins(aether_cook)
