# Aether Audio (Phase 17, docs/design/PHASE_SPECS.md): sounds, the software
# mixer (voices, buses, effects) and 3D audio. Engine-only; it renders into
# buffers, so it builds and tests headless on every platform. Output
# backends pull from it.
add_library(aether_audio STATIC
    src/sound.cpp
    src/dsp.cpp
    src/mixer.cpp
    src/spatial.cpp
)
target_include_directories(aether_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_audio PUBLIC cxx_std_20)
target_link_libraries(aether_audio PUBLIC Aether::Engine)
add_library(Aether::Audio ALIAS aether_audio)
