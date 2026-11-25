add_executable(aether_sprite_grid main.cpp)
target_link_libraries(aether_sprite_grid PRIVATE Aether::Sprite2D)
target_include_directories(aether_sprite_grid PRIVATE "${PROJECT_SOURCE_DIR}/sprite2d/src")
