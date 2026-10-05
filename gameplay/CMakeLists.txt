# Aether Gameplay (Phase 30, docs/design/PHASE_SPECS.md §30): the gameplay
# ability system and RPG toolkit. Gameplay tags first; attributes, effects and
# abilities follow. Engine-only, so it builds and tests headless on every platform.
add_library(aether_gameplay STATIC
    src/gameplay_tag.cpp
    src/tag_container.cpp
    src/tag_query.cpp
    src/tag_library.cpp
    src/attribute_set.cpp
    src/attribute_system.cpp
    src/attribute_library.cpp
    src/gameplay_effect.cpp
    src/effect_system.cpp
    src/effect_library.cpp
    src/gameplay_ability.cpp
    src/ability_system.cpp
)
target_include_directories(aether_gameplay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_gameplay PUBLIC cxx_std_20)
target_link_libraries(aether_gameplay PUBLIC Aether::Engine)
add_library(Aether::Gameplay ALIAS aether_gameplay)
