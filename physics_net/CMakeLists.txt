# Physics over the network (Priority 2, docs/design/PHYSICS_CONSTRAINTS.md "Networking"): the glue between aether_physics and aether_net,
# which may not depend on each other (same layer). Host-authoritative constraints: the host sends constraint snapshots, clients send grab
# requests that the host validates.
add_library(aether_physics_net STATIC
    src/protocol.cpp
    src/grab_policy.cpp
    src/host.cpp
    src/client.cpp
)
target_include_directories(aether_physics_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_physics_net PUBLIC cxx_std_20)
target_link_libraries(aether_physics_net PUBLIC Aether::Physics Aether::Net)
add_library(Aether::PhysicsNet ALIAS aether_physics_net)
