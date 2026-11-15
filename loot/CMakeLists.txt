# Aether Loot: the loot-tables genre kit, built on the inventory kit. A loot table is data (`.aloot`): weighted entries and how many rolls to make. Rolls
# are deterministic (the same seed gives the same drop). Optional: AETHER_KIT_LOOT=OFF leaves it out of the build (and of the player).
add_library(aether_loot STATIC
    src/loot_table.cpp
    src/loot_system.cpp
    src/loot_kit.cpp
)
target_include_directories(aether_loot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_loot PUBLIC cxx_std_20)
target_link_libraries(aether_loot PUBLIC Aether::Inventory)
aether_warnings_as_errors(aether_loot)
add_library(Aether::Loot ALIAS aether_loot)
