// Simple console demo of the Abyss Protocol systems + glTF loading.
// Full Win32 / renderer integration comes later.

#include "abyss_protocol.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    abyss::Game game;
    std::cout << "Abyss Protocol — acoustic + glTF demo\n";

    // Optional: pass a .gltf or .glb path as the first argument
    if (argc > 1) {
        abyss::ModelInfo info;
        std::cout << "Loading model: " << argv[1] << "\n";
        if (game.LoadModel(argv[1], info)) {
            std::cout << "  meshes: " << info.mesh_count
                      << "  primitives: " << info.primitive_count
                      << "  vertices: " << info.vertex_count
                      << "  materials: " << info.material_count << "\n";
            std::cout << "  bounds: (" << info.bounds_min.x << ", " << info.bounds_min.y << ", " << info.bounds_min.z
                      << ") → (" << info.bounds_max.x << ", " << info.bounds_max.y << ", " << info.bounds_max.z << ")\n";
        } else {
            std::cout << "  Failed: " << info.error << "\n";
        }
        std::cout << "\n";
    } else {
        std::cout << "(Tip: pass a .gltf/.glb path to test model loading)\n\n";
    }

    std::cout << "Simulating 10 seconds of movement + sonar...\n\n";

    for (int i = 0; i < 600; ++i) {
        if (i == 60) game.SetSonar(true);
        if (i == 180) game.SetSonar(false);
        if (i == 120) game.ApplyThrust({2.0f, 0.0f, 1.0f});

        game.Step(1.0f / 60.0f);

        if (i % 60 == 0) {
            std::cout << "t=" << (i / 60) << "s  " << game.StatusText() << "\n";
        }
    }

    std::cout << "\nFinal: " << game.StatusText() << "\n";
    return 0;
}
