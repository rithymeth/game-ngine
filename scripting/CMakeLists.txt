add_library(aether_scripting STATIC
    src/luau_host.cpp
    src/world_bindings.cpp
    src/script_system.cpp
    src/script_api.cpp
    src/script_reload.cpp
    src/debugger.cpp
    src/completion.cpp
    src/dap.cpp
    src/debug_bindings.cpp
    src/save_api.cpp
    src/loc_api.cpp
)

target_include_directories(aether_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_scripting PUBLIC cxx_std_20)
# Luau's headers stay private: engine code talks to LuauHost, not to lua.h.
target_link_libraries(aether_scripting PUBLIC Aether::Engine PRIVATE Luau.VM Luau.Compiler Aether::Save Aether::Loc)

add_library(Aether::Scripting ALIAS aether_scripting)
