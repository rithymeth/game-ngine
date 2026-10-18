# Using the installed CMake SDK

The engine installs its module libraries, public headers, dependency libraries,
and CMake package files into one prefix. Configure and install the SDK with:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/path/to/aether-sdk
cmake --build build --config Release
cmake --install build --config Release
```

A consumer project can then link the modules it uses:

```cmake
find_package(Aether CONFIG REQUIRED)

add_executable(game main.cpp)
target_link_libraries(game PRIVATE Aether::Engine Aether::Blueprint)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix if CMake cannot find the
package automatically. Optional engine modules are exported when enabled in
the SDK build configuration.
