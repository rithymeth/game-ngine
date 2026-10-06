add_library(aether_devtools STATIC
    src/profiler.cpp
    src/debug_draw.cpp
    src/crash_win.cpp
)

target_include_directories(aether_devtools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aether_devtools PUBLIC cxx_std_20)
target_link_libraries(aether_devtools PUBLIC Aether::Engine)

# The Windows crash handler writes minidumps via DbgHelp (part of the Windows
# SDK; no third-party dependency).
if(WIN32)
    target_link_libraries(aether_devtools PRIVATE dbghelp)
endif()
add_library(Aether::DevTools ALIAS aether_devtools)
