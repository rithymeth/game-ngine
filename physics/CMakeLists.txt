add_library(aether_physics STATIC
    src/physics_world.cpp
    src/physics_scene.cpp
    src/character.cpp
    src/jolt_job_system_adapter.cpp
)

target_include_directories(aether_physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_physics PUBLIC cxx_std_20)
target_link_libraries(aether_physics PUBLIC Aether::Engine Jolt::Jolt)

add_library(Aether::Physics ALIAS aether_physics)
