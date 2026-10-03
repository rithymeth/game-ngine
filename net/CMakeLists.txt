add_library(aether_net STATIC
    src/loopback.cpp
    src/endpoint.cpp
    src/replication.cpp
    src/rpc.cpp
    src/prediction.cpp
    src/interpolation.cpp
    src/udp.cpp
    src/discovery.cpp
    src/session.cpp
)

target_include_directories(aether_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_net PUBLIC cxx_std_20)
target_link_libraries(aether_net PUBLIC Aether::Engine)
if(WIN32)
    target_link_libraries(aether_net PUBLIC ws2_32)
endif()

add_library(Aether::Net ALIAS aether_net)
