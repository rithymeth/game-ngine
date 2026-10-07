# Windows binary requirements

- Windows x64 with D3D12 support and an AVX2-capable CPU.
- The Microsoft Visual C++ v14 Redistributable for **x64**, at least as recent
  as the MSVC toolset used to build the release. Microsoft provides its
  [supported runtime installer](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/).

The ZIP packages include the engine/game binaries, content and shader compiler
runtime. They do not install the Visual C++ system runtime. If Windows reports
missing `VCRUNTIME140.dll` or `MSVCP140.dll`, install the official x64 runtime
above. Extract the entire ZIP before launching its executable.

The proof binaries are unsigned. Linux/macOS binary packages and measured
minimum GPU/memory specifications are not provided in this proof release.
