# Aether Spawner: the spawning genre kit. A Spawner component on an entity makes prefabs appear around it on a timer, up to a limit alive at once.
# Optional: AETHER_KIT_SPAWNER=OFF leaves it out of the build (and of the player).
add_library(aether_spawner STATIC
    src/spawner.cpp
    src/spawner_system.cpp
    src/spawner_kit.cpp
)
target_include_directories(aether_spawner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_spawner PUBLIC cxx_std_20)
target_link_libraries(aether_spawner PUBLIC Aether::Engine)
aether_warnings_as_errors(aether_spawner)
add_library(Aether::Spawner ALIAS aether_spawner)
