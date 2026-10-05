# Aether Streaming (Phase 21 step 6, docs/design/PHASE_SPECS.md): large
# worlds - partitioning a scene into grid cells, streaming the cells in and
# out around streaming sources, and a floating origin. Engine-only, so it
# builds and tests headless on every platform.
add_library(aether_streaming STATIC
    src/partition.cpp
    src/streamer.cpp
)
target_include_directories(aether_streaming PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_streaming PUBLIC cxx_std_20)
target_link_libraries(aether_streaming PUBLIC Aether::Engine)
add_library(Aether::Streaming ALIAS aether_streaming)
