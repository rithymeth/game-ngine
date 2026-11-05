#include "aether/player/scene_renderer.h"
#include "test_framework.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace aether;

// The player's scene shader (Phase 39.3 added the clear coat, sheen and anisotropy path). Lavapipe can't draw the scene pipeline in CI
// (see the advisory "Play AETHER-01 through Vulkan" step), so the source itself is what these tests can check: that it is what the renderer
// builds, and that glslang, with __spirv__ defined as the engine's own compiler does, accepts both stages in both variants.

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool GlslangAvailable() {
    static const bool available = std::system("glslangValidator --version > /dev/null 2>&1") == 0;
    return available;
}

bool Compiles(const std::string& hlsl, const char* stage, const char* entry, std::string& log) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / (std::string("aether_scene_shader_") + stage);
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "scene.hlsl", out = dir / "log.txt";
    {
        std::ofstream f(src);
        f << "#define __spirv__ 1\n" << hlsl;
    }
    const std::string cmd = std::string("glslangValidator -D -V -S ") + stage + " -e " + entry + " \"" + src.string() + "\" -o \"" +
                            (dir / "out.spv").string() + "\" > \"" + out.string() + "\" 2>&1";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::ifstream l(out);
    log.assign(std::istreambuf_iterator<char>(l), std::istreambuf_iterator<char>());
    std::filesystem::remove_all(dir);
    return ok;
}

} // namespace

AETHER_TEST(SceneShader_SourceIsTheLibraryThenTheSceneShader) {
    const std::string plain = player::SceneShaderSource(false), frame = player::SceneShaderSource(true);
    AETHER_CHECK(!Contains(plain, "#define AETHER_FRAME_DATA"));
    AETHER_CHECK(frame.rfind("#define AETHER_FRAME_DATA 1\n", 0) == 0);
    for (const std::string* s : {&plain, &frame}) {
        const usize library = s->find("AETHER_ADVANCED_PBR_HLSL"), shader = s->find("PSMain");
        AETHER_CHECK(library != std::string::npos && shader != std::string::npos && library < shader); // the helpers come first
        AETHER_CHECK(Contains(*s, "VSMain") && Contains(*s, "SurfaceLightEx") && Contains(*s, "LoadSurfaceExtension"));
        // Every light the scene shader adds goes through the extension-aware term.
        AETHER_CHECK(Contains(*s, "AetherAnisotropicSpecular") && Contains(*s, "AetherSheen") && Contains(*s, "AetherClearCoat"));
    }
    AETHER_CHECK(Contains(frame, "ConstantBuffer<FrameData> g_Frame"));
    AETHER_CHECK(!Contains(plain, "ConstantBuffer<FrameData> g_Frame") || Contains(plain, "#ifdef AETHER_FRAME_DATA"));
}

AETHER_TEST(SceneShader_CompilesWithGlslang) {
    if (!GlslangAvailable()) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    for (const bool with_frame_data : {false, true}) {
        const std::string source = player::SceneShaderSource(with_frame_data);
        std::string log;
        const bool vertex = Compiles(source, "vert", "VSMain", log);
        if (!vertex) std::printf("%s\n", log.c_str());
        AETHER_CHECK(vertex);
        const bool pixel = Compiles(source, "frag", "PSMain", log);
        if (!pixel) std::printf("%s\n", log.c_str());
        AETHER_CHECK(pixel);
    }
}
