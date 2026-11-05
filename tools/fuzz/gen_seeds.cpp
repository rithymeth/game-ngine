// Generates the valid fixtures for the cooked-container fuzz corpora.
//
// Run once (`aether_fuzz_seedgen <dir>`) and commit the output. It exists so the
// seeds can be regenerated after a format change instead of being serialized by
// hand: `tools/fuzz/corpus/amesh` and `tools/fuzz/corpus/atex_cook`.
#include "aether/assets/model_importer.h"
#include "aether/cook/mesh_cook.h"
#include "aether/cook/texture_cook.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace aether;

bool WriteAll(const std::string& path, const std::vector<u8>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    std::printf("%s: %zu bytes\n", path.c_str(), bytes.size());
    return ok;
}

std::vector<u8> TriangleAmesh() {
    assets::MeshPrimitiveData primitive;
    const f32 corners[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (const auto& corner : corners) {
        assets::MeshVertex vertex{};
        vertex.position[0] = corner[0];
        vertex.position[1] = corner[1];
        vertex.position[2] = corner[2];
        vertex.normal[1] = 1.0f;
        vertex.tangent[0] = 1.0f;
        vertex.tangent[3] = 1.0f;
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = {0, 1, 2};
    primitive.bounds_min = Vec3{0.0f, 0.0f, 0.0f};
    primitive.bounds_max = Vec3{1.0f, 1.0f, 0.0f};

    assets::MeshData mesh;
    mesh.primitives.push_back(std::move(primitive));

    cook::MeshCookSettings settings;
    settings.build_lods = true;
    settings.build_meshlets = true;
    settings.build_hlod = true;
    cook::CookedMesh cooked;
    std::string error;
    if (!cook::CookMesh(mesh, settings, cooked, &error)) {
        std::fprintf(stderr, "CookMesh failed: %s\n", error.c_str());
        return {};
    }
    return cook::SaveAmesh(cooked);
}

std::vector<u8> CheckerAtex() {
    constexpr u32 kWidth = 8, kHeight = 8;
    std::vector<u8> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const bool lit = ((x / 2) + (y / 2)) % 2 == 0;
            const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            const u8 value = lit ? 255 : 0;
            rgba[i + 0] = value;
            rgba[i + 1] = value;
            rgba[i + 2] = value;
            rgba[i + 3] = 255;
        }
    }
    cook::CookedTexture cooked;
    cooked.format = cook::ChooseTextureFormat(rgba, /*normal_map=*/false);
    cooked.srgb = true;
    cooked.normal_map = false;
    cooked.width = kWidth;
    cooked.height = kHeight;
    cooked.mips = cook::GenerateMips(rgba, kWidth, kHeight, /*srgb=*/true, /*normal_map=*/false);
    return cook::SaveAtex(cooked);
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string sep = dir.empty() || dir.back() == '/' ? "" : "/";
    bool ok = true;
    ok = WriteAll(dir + sep + "amesh/valid_triangle.amesh", TriangleAmesh()) && ok;
    ok = WriteAll(dir + sep + "atex_cook/valid_checker.atex", CheckerAtex()) && ok;
    return ok ? 0 : 1;
}
