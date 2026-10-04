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
# Scenes' scripts and Blueprints run in the player (Phase 26 step 4); Luau
# only when the build has it.
target_link_libraries(aether_game PUBLIC Aether::Blueprint Aether::Sprite2D)
if(TARGET Aether::Scripting)
    target_link_libraries(aether_game PRIVATE Aether::Scripting)
    target_compile_definitions(aether_game PRIVATE AETHER_GAME_SCRIPTING=1)
else()
    target_compile_definitions(aether_game PRIVATE AETHER_GAME_SCRIPTING=0)
endif()
add_library(Aether::Game ALIAS aether_game)

add_executable(aether_player main.cpp)
target_link_libraries(aether_player PRIVATE Aether::Game)
aether_link_platform_plugins(aether_player)

# See sandbox/CMakeLists.txt: dxcompiler.dll is a load-time dependency of
# anything linking aether_engine on Windows, so it goes beside the player
# (and Package copies it along with the player).
if(AETHER_DXCOMPILER_DLL)
    add_custom_command(TARGET aether_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:aether_player>/dxcompiler.dll"
    )
endif()
