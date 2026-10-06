# aether_pak: create, list, extract and verify .apak archives (Phase 25).
add_executable(aether_pak main.cpp)
target_link_libraries(aether_pak PRIVATE Aether::Engine)
