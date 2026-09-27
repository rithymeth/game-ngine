add_library(aether_scripting STATIC
    src/luau_host.cpp
    src/world_bindings.cpp
    src/script_system.cpp
)

target_include_directories(aether_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_scripting PUBLIC cxx_std_20)
# Luau's headers stay private: engine code talks to LuauHost, not to lua.h.
target_link_libraries(aether_scripting PUBLIC Aether::Engine PRIVATE Luau.VM Luau.Compiler)

add_library(Aether::Scripting ALIAS aether_scripting)
