# Aether Vehicles: the wheeled-vehicle genre kit, built on the physics module and Jolt's vehicle constraint. A Vehicle component on an entity with a box
# collider and a dynamic body gives it four wheels on springs, an engine, a gearbox and brakes; a VehicleInput component (or a script) drives it. Needs
# physics: AETHER_KIT_VEHICLES=OFF (or a build without physics) leaves it out of the build and of the player.
add_library(aether_vehicles STATIC
    src/vehicle.cpp
    src/vehicle_system.cpp
    src/vehicles_kit.cpp
)
target_include_directories(aether_vehicles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_vehicles PUBLIC cxx_std_20)
target_link_libraries(aether_vehicles PUBLIC Aether::Physics)
aether_warnings_as_errors(aether_vehicles)
add_library(Aether::Vehicles ALIAS aether_vehicles)
