# Aether Crafting: the recipes genre kit, built on the inventory kit. A recipe is data (`.arecipe`): the items it takes and
# the items it makes. Optional: AETHER_KIT_CRAFTING=OFF leaves it out of the build (and of the player).
add_library(aether_crafting STATIC
    src/recipe_def.cpp
    src/crafting_system.cpp
    src/crafting_kit.cpp
)
target_include_directories(aether_crafting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_crafting PUBLIC cxx_std_20)
target_link_libraries(aether_crafting PUBLIC Aether::Inventory)
aether_warnings_as_errors(aether_crafting)
add_library(Aether::Crafting ALIAS aether_crafting)
