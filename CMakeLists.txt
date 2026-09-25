cmake_minimum_required(VERSION 3.20)
project(Aether LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(AETHER_BUILD_TESTS "Build Aether unit tests" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /arch:AVX2 /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -mavx2 -mfma)
endif()

option(AETHER_BUILD_SANDBOX "Build the Aether sandbox app" ON)
option(AETHER_BUILD_PHYSICS "Build the Jolt-backed physics module" ON)
option(AETHER_BUILD_EDITOR "Build the Dear ImGui editor app" ON)

include(FetchContent)

if(AETHER_BUILD_PHYSICS)
    # Jolt defaults to the static MSVC runtime (/MT) on MSVC; our own targets
    # use the default dynamic runtime (/MD), and linking a /MT static lib
    # into a /MD executable is a hard CRT conflict (duplicate symbols). Force
    # Jolt onto the dynamic runtime to match before it configures.
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.6.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(JoltPhysics)
endif()

if(AETHER_BUILD_EDITOR)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    # Dear ImGui doesn't ship a CMakeLists.txt (it's meant to be dropped into
    # the consumer's build), so we define the library ourselves: core +
    # Win32/DX12 backends, matching this project's window/graphics layer.
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC d3d12 dxgi)
    add_library(ImGui::ImGui ALIAS imgui)
endif()

add_subdirectory(engine)

if(AETHER_BUILD_PHYSICS)
    add_subdirectory(physics)
endif()

if(AETHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(AETHER_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

if(AETHER_BUILD_PHYSICS AND AETHER_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
