# Aether Inventory (Phase 30 step 7a, docs/design/PHASE_SPECS.md §30.8): the
# inventory and items genre kit, built on the gameplay module's effects. Optional:
# AETHER_KIT_INVENTORY=OFF leaves it out of the build (and of the player).
add_library(aether_inventory STATIC
    src/item_def.cpp
    src/inventory.cpp
    src/inventory_system.cpp
    src/inventory_kit.cpp
    src/inventory_library.cpp
)
target_include_directories(aether_inventory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_inventory PUBLIC cxx_std_20)
target_link_libraries(aether_inventory PUBLIC Aether::Gameplay)
aether_warnings_as_errors(aether_inventory)
add_library(Aether::Inventory ALIAS aether_inventory)
