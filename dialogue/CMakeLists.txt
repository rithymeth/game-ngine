# Aether Dialogue: the conversations genre kit. A conversation is data (`.adialogue`): nodes that say a line and lead to the next node or
# offer choices. Optional: AETHER_KIT_DIALOGUE=OFF leaves it out of the build (and of the player).
add_library(aether_dialogue STATIC
    src/dialogue_def.cpp
    src/dialogue_system.cpp
    src/dialogue_kit.cpp
)
target_include_directories(aether_dialogue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_dialogue PUBLIC cxx_std_20)
target_link_libraries(aether_dialogue PUBLIC Aether::Engine)
aether_warnings_as_errors(aether_dialogue)
add_library(Aether::Dialogue ALIAS aether_dialogue)
