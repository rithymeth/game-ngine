cmake_minimum_required(VERSION 3.21)

# This game can be included by Aether's root build, or built independently
# while compiling the exact Aether source files it uses from this checkout.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(Lanternwake VERSION 1.0 LANGUAGES CXX)
    include(CTest)
    set(LANTERNWAKE_STANDALONE ON)
endif()

get_filename_component(LANTERNWAKE_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LANTERNWAKE_DEPS_DIR "${LANTERNWAKE_ENGINE_ROOT}/build/deps" CACHE PATH "Official SDL2/stb checkout directory")
option(LANTERNWAKE_SINGLE_FILE "Embed the browser runtime and game assets in one HTML file" ON)

if(TARGET Aether::Engine)
    set(LANTERNWAKE_ENGINE_TARGET Aether::Engine)
    set(LANTERNWAKE_PRESENTATION_TARGETS Aether::UI Aether::Audio)
else()
    if(NOT EXISTS "${LANTERNWAKE_DEPS_DIR}/stb/stb_image.h")
        message(FATAL_ERROR "stb headers are missing. Run games/LANTERNWAKE/tools/bootstrap_deps.sh first.")
    endif()
    add_library(lanternwake_aether STATIC
        "${LANTERNWAKE_ENGINE_ROOT}/engine/src/ecs/archetype.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/engine/src/assets/image.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/engine/src/assets/png_writer.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/engine/src/assets/stb_image_impl.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/engine/src/core/log.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/audio/src/sound.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/ui/src/font.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/ui/src/draw.cpp"
        "${LANTERNWAKE_ENGINE_ROOT}/ui/src/bidi.cpp"
    )
    target_include_directories(lanternwake_aether PUBLIC
        "${LANTERNWAKE_ENGINE_ROOT}/engine/include"
        "${LANTERNWAKE_ENGINE_ROOT}/audio/include"
        "${LANTERNWAKE_ENGINE_ROOT}/ui/include"
    )
    target_include_directories(lanternwake_aether PRIVATE "${LANTERNWAKE_DEPS_DIR}/stb")
    target_compile_features(lanternwake_aether PUBLIC cxx_std_20)
    set(LANTERNWAKE_ENGINE_TARGET lanternwake_aether)
endif()

add_executable(lanternwake
    src/main.cpp
    src/game.cpp
    src/renderer.cpp
    src/audio.cpp
)
target_compile_features(lanternwake PRIVATE cxx_std_20)
target_link_libraries(lanternwake PRIVATE ${LANTERNWAKE_ENGINE_TARGET} ${LANTERNWAKE_PRESENTATION_TARGETS})
target_compile_definitions(lanternwake PRIVATE
    LANTERNWAKE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

if(EMSCRIPTEN)
    # The host layer and all Aether/game C++ sources are shared with native.
    # Emscripten supplies its own SDL2 port, not a replacement game runtime.
    target_compile_options(lanternwake PRIVATE -sUSE_SDL=2)
    target_link_options(lanternwake PRIVATE
        -sUSE_SDL=2
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=1048576
        -sFORCE_FILESYSTEM=1
        -sENVIRONMENT=web
        -sEXIT_RUNTIME=0
    )
    if(LANTERNWAKE_SINGLE_FILE)
        # embed-file puts assets inside the wasm; SINGLE_FILE embeds that wasm
        # in the HTML. Unlike preload-file, this leaves no external .data file.
        target_link_options(lanternwake PRIVATE -sSINGLE_FILE=1
            "--embed-file" "${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets")
    else()
        target_link_options(lanternwake PRIVATE
            "--preload-file" "${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell.html")
        target_link_options(lanternwake PRIVATE "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell.html")
        set_property(TARGET lanternwake APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/shell.html")
    endif()
    file(GLOB_RECURSE LANTERNWAKE_ASSETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
    set_property(TARGET lanternwake APPEND PROPERTY LINK_DEPENDS ${LANTERNWAKE_ASSETS})
    set_target_properties(lanternwake PROPERTIES OUTPUT_NAME index SUFFIX ".html")
else()
    find_package(SDL2 QUIET CONFIG)
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(lanternwake PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(lanternwake PRIVATE SDL2::SDL2)
        if(WIN32)
            get_target_property(LANTERNWAKE_SDL_TARGET_TYPE SDL2::SDL2 TYPE)
            if(LANTERNWAKE_SDL_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
                # An imported SDL2 package knows the DLL's actual location.
                # Copy it for explicit target builds as well as all-target
                # builds, including when a copied DLL has been removed.
                add_custom_target(lanternwake_sdl_runtime
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:lanternwake>"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:lanternwake>"
                    COMMENT "Copying the SDL2 runtime beside Lanternwake"
                    VERBATIM)
                add_dependencies(lanternwake lanternwake_sdl_runtime)
            endif()
        endif()
    else()
        find_path(LANTERNWAKE_SDL_INCLUDE_DIR SDL.h
            HINTS "${LANTERNWAKE_DEPS_DIR}/SDL2/include"
            PATH_SUFFIXES SDL2)
        find_library(LANTERNWAKE_SDL_LIBRARY NAMES SDL2 SDL2-2.0)
        if(UNIX AND NOT APPLE AND NOT LANTERNWAKE_SDL_LIBRARY)
            # Linux runtime-only installations have the soname, but no
            # development-package libSDL2.so symlink. Use official headers.
            find_file(LANTERNWAKE_SDL_LIBRARY NAMES libSDL2-2.0.so.0
                PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu
                      /usr/lib/aarch64-linux-gnu /lib/x86_64-linux-gnu
                      /lib/aarch64-linux-gnu)
        endif()
        if(LANTERNWAKE_SDL_INCLUDE_DIR AND LANTERNWAKE_SDL_LIBRARY)
            target_include_directories(lanternwake PRIVATE "${LANTERNWAKE_SDL_INCLUDE_DIR}")
            target_link_libraries(lanternwake PRIVATE "${LANTERNWAKE_SDL_LIBRARY}")
        elseif(EXISTS "${LANTERNWAKE_DEPS_DIR}/SDL2/CMakeLists.txt")
            # Portable fallback for Windows/macOS without an installed SDL2.
            set(SDL_SHARED OFF CACHE BOOL "Build shared SDL" FORCE)
            set(SDL_STATIC ON CACHE BOOL "Build static SDL" FORCE)
            set(SDL_TEST OFF CACHE BOOL "Build SDL tests" FORCE)
            add_subdirectory("${LANTERNWAKE_DEPS_DIR}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)
            target_link_libraries(lanternwake PRIVATE SDL2::SDL2-static)
        else()
            message(FATAL_ERROR "SDL2 was not found. Install SDL2 or run tools/bootstrap_deps.sh.")
        endif()
    endif()
    add_custom_target(lanternwake_assets ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:lanternwake>/assets"
        COMMENT "Copying Lanternwake assets beside the native game")
    # CMP0112 is NEW with our CMake 3.21 minimum: TARGET_FILE_DIR does not
    # create a reverse dependency on the executable. Explicit target builds
    # therefore copy current assets too, without forming a dependency cycle.
    add_dependencies(lanternwake lanternwake_assets)
endif()

if(MSVC)
    target_compile_options(lanternwake PRIVATE /W4 /permissive-)
else()
    target_compile_options(lanternwake PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Keep the simulation tests in their own binary. No renderer or SDL dependency.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/game_tests.cpp")
    add_executable(lanternwake_tests tests/game_tests.cpp src/game.cpp)
    target_compile_features(lanternwake_tests PRIVATE cxx_std_20)
    target_include_directories(lanternwake_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lanternwake_tests PRIVATE ${LANTERNWAKE_ENGINE_TARGET})
    add_test(NAME lanternwake_gameplay COMMAND lanternwake_tests)
endif()
