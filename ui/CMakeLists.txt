# Aether UI (Phase 18, docs/design/PHASE_SPECS.md): the runtime game UI - a
# retained widget tree, layout, input and a draw list the renderer draws.
# Engine-only, so it builds and tests headless on every platform.
add_library(aether_ui STATIC
    src/draw.cpp
    src/widget.cpp
    src/panels.cpp
    src/basic.cpp
    src/viewport.cpp
    src/controls.cpp
    src/input.cpp
    src/theme.cpp
    src/layout_file.cpp
    src/binding.cpp
    src/animation.cpp
    src/widget_system.cpp
)
target_include_directories(aether_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_ui PUBLIC cxx_std_20)
target_link_libraries(aether_ui PUBLIC Aether::Engine)
add_library(Aether::UI ALIAS aether_ui)
