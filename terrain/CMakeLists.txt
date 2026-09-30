add_library(aether_terrain STATIC
    src/terrain.cpp
    src/splat.cpp
    src/renderer.cpp
    src/foliage.cpp
)

target_include_directories(aether_terrain PUBLIC /include)
target_compile_features(aether_terrain PUBLIC cxx_std_20)
target_link_libraries(aether_terrain PUBLIC Aether::Engine Aether::Gfx)

add_library(Aether::Terrain ALIAS aether_terrain)
