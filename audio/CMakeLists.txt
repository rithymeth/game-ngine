# Aether Audio (Phase 17, docs/design/PHASE_SPECS.md): sounds and their
# decoders, streaming, the software mixer (voices, buses, effects), 3D audio
# and sound cues. Engine-only; it renders into buffers, so it builds and
# tests headless on every platform. Output backends pull from it.
add_library(aether_audio STATIC
    src/sound.cpp
    src/dsp.cpp
    src/mixer.cpp
    src/spatial.cpp
    src/decoders.cpp
    src/codec_impl.cpp
    src/cue.cpp
    src/cue_player.cpp
)
target_include_directories(aether_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_audio PUBLIC cxx_std_20)
target_link_libraries(aether_audio PUBLIC Aether::Engine)
# stb_vorbis, dr_flac and dr_wav are single-file decoders, compiled once in
# codec_impl.cpp. Third-party code: don't hold it to our warning flags.
target_include_directories(aether_audio PRIVATE ${stb_SOURCE_DIR} ${dr_libs_SOURCE_DIR})
if(MSVC)
    set_source_files_properties(src/codec_impl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/codec_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_library(Aether::Audio ALIAS aether_audio)
