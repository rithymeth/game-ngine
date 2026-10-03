# Aether Net (Phase 22, docs/design/PHASE_SPECS.md): the network transport -
# datagram sockets (UDP, and an in-memory network with simulated latency
# and loss for tests and the editor), connections with reliable and
# unreliable channels, and hosts that accept or make connections - and
# replication: delta snapshots of networked entities.
add_library(aether_net STATIC
    src/socket.cpp
    src/connection.cpp
    src/host.cpp
    src/replication.cpp
    src/rpc.cpp
    src/interpolation.cpp
    src/prediction.cpp
)
target_include_directories(aether_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_net PUBLIC cxx_std_20)
target_link_libraries(aether_net PUBLIC Aether::Engine)
if(WIN32)
    target_link_libraries(aether_net PRIVATE ws2_32)
endif()
add_library(Aether::Net ALIAS aether_net)
