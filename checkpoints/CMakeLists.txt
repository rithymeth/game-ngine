# Aether Checkpoints: the respawn-points genre kit. A Checkpoint component marks a place; when the player reaches it, it becomes the last one, kept
# in the game's save bag (so it is saved with the slot), and a script can put the player back there. Optional: AETHER_KIT_CHECKPOINTS=OFF leaves it
# out of the build (and of the player).
add_library(aether_checkpoints STATIC
    src/checkpoint.cpp
    src/checkpoint_system.cpp
    src/checkpoints_kit.cpp
)
target_include_directories(aether_checkpoints PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_checkpoints PUBLIC cxx_std_20)
target_link_libraries(aether_checkpoints PUBLIC Aether::Engine Aether::Save)
aether_warnings_as_errors(aether_checkpoints)
add_library(Aether::Checkpoints ALIAS aether_checkpoints)
