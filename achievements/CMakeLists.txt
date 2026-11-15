# Aether Achievements: the goals genre kit. An achievement is data (`.aachievement`): a name, a title and a target count. Progress lives in the
# game's save bag, so it is saved and loaded with the slot. Optional: AETHER_KIT_ACHIEVEMENTS=OFF leaves it out of the build (and of the player).
add_library(aether_achievements STATIC
    src/achievement_def.cpp
    src/achievement_system.cpp
    src/achievements_kit.cpp
)
target_include_directories(aether_achievements PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_achievements PUBLIC cxx_std_20)
target_link_libraries(aether_achievements PUBLIC Aether::Engine Aether::Save)
aether_warnings_as_errors(aether_achievements)
add_library(Aether::Achievements ALIAS aether_achievements)
