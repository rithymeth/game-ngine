cmake_minimum_required(VERSION 3.21)
project(Hearthline LANGUAGES CXX)
include(CTest)

add_library(hearthline_core STATIC src/game.cpp src/combat.cpp src/save.cpp)
target_compile_features(hearthline_core PUBLIC cxx_std_20)
target_include_directories(hearthline_core PUBLIC src)

add_executable(hearthline_sim src/main.cpp)
target_link_libraries(hearthline_sim PRIVATE hearthline_core)
if(MSVC)
    target_compile_options(hearthline_core PRIVATE /W4)
else()
    target_compile_options(hearthline_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(hearthline_tests tests/game_tests.cpp)
    target_link_libraries(hearthline_tests PRIVATE hearthline_core)
    add_test(NAME hearthline_simulation COMMAND hearthline_tests)
endif()

option(HEARTHLINE_BUILD_VISUAL "Build the SDL2 graphical prototype" OFF)
if(HEARTHLINE_BUILD_VISUAL)
    add_executable(hearthline_visual src/visual_main.cpp)
    target_link_libraries(hearthline_visual PRIVATE hearthline_core)
    if(EMSCRIPTEN)
        # Emscripten provides the SDL2 port; no native SDL2 package is needed.
        target_compile_options(hearthline_visual PRIVATE -sUSE_SDL=2)
        target_link_options(hearthline_visual PRIVATE
            -sUSE_SDL=2
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=134217728
            -sSTACK_SIZE=1048576
            -sSINGLE_FILE=1
            -sENVIRONMENT=web
            -sEXIT_RUNTIME=0
            -sEXPORTED_RUNTIME_METHODS=ccall
            "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
        )
        set_target_properties(hearthline_visual PROPERTIES OUTPUT_NAME index SUFFIX ".html")
        set_property(TARGET hearthline_visual APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html")
    else()
        find_package(SDL2 REQUIRED CONFIG)
        if(TARGET SDL2::SDL2)
            target_link_libraries(hearthline_visual PRIVATE SDL2::SDL2)
        elseif(TARGET SDL2::SDL2-static)
            target_link_libraries(hearthline_visual PRIVATE SDL2::SDL2-static)
        else()
            message(FATAL_ERROR "SDL2 CMake package must export an SDL2 target")
        endif()
    endif()
    if(MSVC)
        target_compile_options(hearthline_visual PRIVATE /W4)
    else()
        target_compile_options(hearthline_visual PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
