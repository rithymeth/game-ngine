# Aether Blueprints (Phase 12, docs/design/PHASE_SPECS.md): the graph model,
# node library, validation, and (from step 2) the compiler and VM. Built on
# the engine only, so it builds and tests headless on every platform.
add_library(aether_blueprint STATIC
    src/types.cpp
    src/graph.cpp
    src/nodes.cpp
    src/validate.cpp
    src/bytecode.cpp
    src/compiler.cpp
    src/vm.cpp
    src/system.cpp
)
target_include_directories(aether_blueprint PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_blueprint PUBLIC cxx_std_20)
target_link_libraries(aether_blueprint PUBLIC Aether::Engine)
add_library(Aether::Blueprint ALIAS aether_blueprint)
