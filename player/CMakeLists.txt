# Phase 25 step 4: the player - a cooked game without the editor.
# aether_game is its runtime (mounting the paks, loading the startup scene,
# the frame loop); aether_player is the executable around it.
add_library(aether_game STATIC src/game.cpp)
target_include_directories(aether_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aether_game PUBLIC Aether::Engine)
if(TARGET Aether::Physics)
    target_link_libraries(aether_game PRIVATE Aether::Physics)
    target_compile_definitions(aether_game PRIVATE AETHER_GAME_PHYSICS=1)
else()
    target_compile_definitions(aether_game PRIVATE AETHER_GAME_PHYSICS=0)
endif()
add_library(Aether::Game ALIAS aether_game)

add_executable(aether_player main.cpp)
target_link_libraries(aether_player PRIVATE Aether::Game)
aether_link_platform_plugins(aether_player)
