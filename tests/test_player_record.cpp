#include "aether/assets/procedural_mesh.h"
#include "aether/cook/cooker.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/sequence.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets::procedural;
namespace fs = std::filesystem;

// `aether_player --record-dir` films a normal run: a scene's own sequence plays at the film's frame rate and every frame is written. A red box slides across
// the picture over one second (a Transform track of a sequence the scene plays by itself); recorded at 8 frames a second the box is further along each frame,
// and a camera path file that is bad is refused with a reason and a failing exit code.
#if defined(AETHER_TEST_PLAYER_PATH) && !defined(_WIN32)
namespace {

reflect::Json Entity(const std::string& guid, reflect::Json components) { return {{"guid", guid}, {"components", std::move(components)}}; }
reflect::Json At(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", {1.0f, 1.0f, 1.0f}}};
}

fs::path CookSlidingBox(const fs::path& root) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    fs::create_directories(root / "Content/Sequences");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "RecordScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array({"Sequences/"});
    std::ofstream(root / "RecordScene.aproject") << project.dump(2);
    MeshBuilder box;
    box.Box(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f));
    ProceduralMesh mesh;
    assets::MaterialData red;
    red.base_color = Vec4(0.95f, 0.05f, 0.05f, 1.0f);
    red.roughness = 0.9f;
    red.metallic = 0.0f;
    mesh.materials.push_back(red);
    mesh.mesh.primitives.push_back(box.Finish(Shading::Flat, UvMode::BoxProjected, 0));
    const std::vector<u8> glb = EncodeGlb(mesh);
    std::ofstream(root / "Content/models/box.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    // A shrub (a swaying plant of a few hundred triangles) a few metres off: small enough on this 160 x 90 picture to take its far level.
    const std::vector<u8> shrub_glb = EncodeGlb(GenerateVariant(ProceduralKind::Shrub, 1, 3));
    std::ofstream(root / "Content/models/shrub.glb", std::ios::binary).write(reinterpret_cast<const char*>(shrub_glb.data()), static_cast<std::streamsize>(shrub_glb.size()));
    const std::string box_guid = "a0000000-0000-4000-8000-0000000000b1";
    // The sequence: the box's x goes from -2.5 to 2.5 over one second.
    seq::LevelSequence sequence;
    sequence.name = "slide";
    sequence.fps = 8.0f;
    sequence.duration = 1.0f;
    seq::Track track;
    track.id = "slide";
    track.type = seq::TrackType::Transform;
    EntityGuid guid;
    AETHER_CHECK(ParseEntityGuid(box_guid, guid));
    track.binding = guid;
    track.channels = {seq::Channel{"x", {seq::Key{0.0f, -2.5f}, seq::Key{1.0f, 2.5f}}}, seq::Channel{"y", {}}, seq::Channel{"z", {}}};
    sequence.tracks.push_back(track);
    std::string error;
    AETHER_CHECK(seq::SaveSequence((root / "Content/Sequences/slide.asequence").string(), sequence, &error));
    const reflect::Json entities = {
        Entity(assets::ToString(assets::NewAssetGuid()),
               {{"Transform", At({0.0f, 0.0f, 6.0f})},
                {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 50.0f}, {"ortho_height", 10.0f}, {"priority", 0}}}}),
        Entity(assets::ToString(assets::NewAssetGuid()), {{"Transform", At({0, 0, 0}, {-0.3f, 0.2f, 0.05f, 0.93f})},
                                                          {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        // Two more boxes the camera cannot see (far to the side, and behind it): drawn or left out, the picture is the same.
        Entity(assets::ToString(assets::NewAssetGuid()), {{"Transform", At({60.0f, 0.0f, 0.0f})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/box.glb"}}}}),
        Entity(assets::ToString(assets::NewAssetGuid()), {{"Transform", At({0.0f, 0.0f, 20.0f})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/box.glb"}}}}),
        Entity(assets::ToString(assets::NewAssetGuid()), {{"Transform", At({0.6f, -1.3f, -2.0f})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/shrub.glb"}}}}),
        Entity(box_guid, {{"Transform", At({-2.5f, 0.0f, 0.0f})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/box.glb"}}}}),
        Entity(assets::ToString(assets::NewAssetGuid()),
               {{"Transform", At({0, 0, 0})},
                {"SequenceComponent", {{"$v", 1}, {"sequence", "Sequences/slide.asequence"}, {"auto_play", true}, {"loop", false}, {"rate", 1.0f}, {"destroy_when_finished", false}}}})};
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "RecordScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

// The red box's centre column in a BMP written by the player (32-bit, top-down), or -1 when there is no red.
int RedCentreColumn(const fs::path& file, int& width) {
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 54) return -1;
    width = *reinterpret_cast<const int*>(&bytes[18]);
    const int height = -*reinterpret_cast<const int*>(&bytes[22]);
    if (width <= 0 || height <= 0 || bytes.size() < 54 + static_cast<usize>(width) * height * 4) return -1;
    double sum = 0.0;
    usize count = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const unsigned char* p = &bytes[54 + (static_cast<usize>(y) * width + x) * 4]; // B, G, R, A
            if (p[2] > p[1] + 80 && p[2] > p[0] + 80) { sum += x; ++count; }
        }
    return count > 20 ? static_cast<int>(sum / static_cast<double>(count)) : -1;
}

std::vector<unsigned char> ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

AETHER_TEST(PlayerRecord_LeavingOutWhatTheCameraCannotSeeDoesNotChangeThePicture) {
    const fs::path root = fs::temp_directory_path() / "aether_player_cull" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookSlidingBox(root);
    if (pak.empty()) return;
    // The same film twice on the fixed film clock: with the view culling off (AETHER_RENDER_CULLING=0) and on. Every frame is the same bytes.
    for (const char* mode : {"0", "1"}) {
        const std::string run = std::string("AETHER_RENDER_CULLING=") + mode + " \"" + AETHER_TEST_PLAYER_PATH + "\" --pak \"" + pak.string() + "\" --size 160x90 --user-dir \"" +
                                (root / "user").string() + "\" --record-dir \"" + (root / (std::string("cull") + mode)).string() + "\" --record-fps 8 --frames 6 > /dev/null 2>&1";
        AETHER_CHECK(std::system(run.c_str()) == 0);
    }
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05d.bmp", i);
        const std::vector<unsigned char> off = ReadAll(root / "cull0" / name), on = ReadAll(root / "cull1" / name);
        AETHER_CHECK(!off.empty());
        AETHER_CHECK(off == on);
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(PlayerRecord_APlantFarAwayTakesItsFarLevelAndLooksAlmostTheSame) {
    const fs::path root = fs::temp_directory_path() / "aether_player_lod" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookSlidingBox(root);
    if (pak.empty()) return;
    // The same film with far levels off (AETHER_RENDER_LOD=0) and on: the picture changes (the shrub is drawn from fewer triangles) and changes little.
    for (const char* mode : {"0", "1"}) {
        const std::string run = std::string("AETHER_RENDER_LOD=") + mode + " \"" + AETHER_TEST_PLAYER_PATH + "\" --pak \"" + pak.string() + "\" --size 160x90 --user-dir \"" +
                                (root / "user").string() + "\" --record-dir \"" + (root / (std::string("lod") + mode)).string() + "\" --record-fps 8 --frames 3 > /dev/null 2>&1";
        AETHER_CHECK(std::system(run.c_str()) == 0);
    }
    usize differing = 0, total = 0;
    const std::vector<unsigned char> full = ReadAll(root / "lod0/frame_00002.bmp"), far = ReadAll(root / "lod1/frame_00002.bmp");
    AETHER_CHECK(full.size() > 54 && full.size() == far.size());
    for (usize i = 54; i + 3 < full.size() && i + 3 < far.size(); i += 4) {
        ++total;
        if (full[i] != far[i] || full[i + 1] != far[i + 1] || full[i + 2] != far[i + 2]) ++differing;
    }
    std::printf("    far level: %zu of %zu pixels differ (%.2f%%)\n", differing, total, total ? 100.0 * static_cast<double>(differing) / static_cast<double>(total) : 0.0);
    AETHER_CHECK(differing > 0);            // the far level was drawn
    AETHER_CHECK(differing * 10 < total);   // and under a tenth of the picture changed
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(PlayerRecord_ASceneSequenceIsFilmedFrameByFrame) {
    const fs::path root = fs::temp_directory_path() / "aether_player_record" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookSlidingBox(root);
    if (pak.empty()) return;
    const fs::path frames = root / "frames";
    const std::string run = std::string("\"") + AETHER_TEST_PLAYER_PATH + "\" --pak \"" + pak.string() + "\" --size 160x90 --user-dir \"" + (root / "user").string() +
                            "\" --record-dir \"" + frames.string() + "\" --record-fps 8 --frames 8 > /dev/null 2>&1";
    AETHER_CHECK(std::system(run.c_str()) == 0);
    std::vector<int> columns;
    int width = 0;
    for (int i = 0; i < 8; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05d.bmp", i);
        AETHER_CHECK(fs::exists(frames / name));
        columns.push_back(RedCentreColumn(frames / name, width));
    }
    AETHER_CHECK(!fs::exists(frames / "frame_00008.bmp")); // exactly the frames asked for
    std::string line;
    for (const int c : columns) line += std::to_string(c) + " ";
    std::printf("    the red box's centre column in the 8 frames (of %d px): %s\n", width, line.c_str());
    AETHER_CHECK(columns.size() == 8 && columns.front() >= 0 && columns.back() >= 0);
    bool rising = true;
    for (usize i = 1; i < columns.size(); ++i) rising = rising && columns[i] > columns[i - 1];
    AETHER_CHECK(rising);                                  // further along every frame
    AETHER_CHECK(columns.front() < width / 2 && columns.back() > width / 2); // from the left of the picture to the right of it
    // Recording needs to be told how many frames, and a bad camera path is refused.
    const std::string needs_frames = std::string("\"") + AETHER_TEST_PLAYER_PATH + "\" --pak \"" + pak.string() + "\" --user-dir \"" + (root / "user").string() +
                                     "\" --record-dir \"" + (root / "none").string() + "\" > /dev/null 2>&1";
    AETHER_CHECK(std::system(needs_frames.c_str()) != 0);
    std::ofstream(root / "bad.json") << R"({"keys": [{"t": 1, "position": [0,0,0]}, {"t": 1, "position": [0,0,0]}]})";
    const std::string bad_path = std::string("\"") + AETHER_TEST_PLAYER_PATH + "\" --pak \"" + pak.string() + "\" --user-dir \"" + (root / "user").string() +
                                 "\" --camera-path \"" + (root / "bad.json").string() + "\" --frames 2 > /dev/null 2>&1";
    AETHER_CHECK(std::system(bad_path.c_str()) != 0);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
#endif
