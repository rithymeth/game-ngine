# M8.9: the dedicated server. aether_server_core is the headless host and test
# client (no player, no renderer: tests use it directly); aether_server wraps
# it around a cooked game in a headless player::Game; aether_netclient is the
# headless client used by CI and to smoke-test a deployed server.
add_library(aether_server_core STATIC src/server_config.cpp src/dedicated_server.cpp)
target_include_directories(aether_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_server_core PUBLIC Aether::Net Aether::Engine)
add_library(Aether::ServerCore ALIAS aether_server_core)

add_executable(aether_server main.cpp)
target_link_libraries(aether_server PRIVATE Aether::ServerCore Aether::Game)

add_executable(aether_netclient netclient_main.cpp)
target_link_libraries(aether_netclient PRIVATE Aether::ServerCore)
