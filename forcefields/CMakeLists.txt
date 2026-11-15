# Aether Force Fields: the physics-volumes genre kit, built on the physics module. A ForceField component on an entity pushes the dynamic bodies inside its
# volume (wind, gravity wells, whirlpools, water buoyancy and drag), and an explosion call pushes everything near a point. Needs physics:
# AETHER_KIT_FORCEFIELDS=OFF (or a build without physics) leaves it out of the build and of the player.
add_library(aether_forcefields STATIC
    src/force_field.cpp
    src/force_field_system.cpp
    src/forcefields_kit.cpp
)
target_include_directories(aether_forcefields PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_forcefields PUBLIC cxx_std_20)
target_link_libraries(aether_forcefields PUBLIC Aether::Physics)
aether_warnings_as_errors(aether_forcefields)
add_library(Aether::ForceFields ALIAS aether_forcefields)
