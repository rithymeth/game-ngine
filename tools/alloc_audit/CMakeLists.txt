# aether_alloc_audit (Phase 38 step 7): hot paths under an allocation counter. It defines global operator new for
# itself only, so it is a program of its own rather than part of aether_tests.
add_executable(aether_alloc_audit main.cpp)
target_link_libraries(aether_alloc_audit PRIVATE Aether::Engine Aether::AI)
add_test(NAME alloc_audit COMMAND aether_alloc_audit)
