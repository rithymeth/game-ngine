#include "aether/assets/gltf_loader.h"
#include "aether/assets/model_importer.h"
#include "aether/assets/texture_importer.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets;
namespace stdfs = std::filesystem;

// Roadmap row A05: "require actionable failures for malformed inputs". An importer that says only "not a readable image" or "couldn't load" leaves an artist guessing; and one that says "ok" for a
// file that imported as nothing is worse. These feed it the failures that really happen and require that the message names the cause and the fix, and that a good file says nothing.

namespace {

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
bool AnyWarning(const ImportResult& r, const char* needle) {
    return std::any_of(r.warnings.begin(), r.warnings.end(), [&](const std::string& w) { return Has(w, needle); });
}

std::string Base64(const std::vector<u8>& bytes) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 chunk = (bytes[i] << 16) | ((i + 1 < bytes.size() ? bytes[i + 1] : 0) << 8) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

// A one-triangle glTF with its data embedded: positions, optional normals, and u16 indices. `tweak` changes a part of it.
struct Triangle {
    std::vector<f32> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<f32> normals{0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<u16> indices{0, 1, 2};
    bool with_normals = true;
    int mode = 4;
    bool with_position_attribute = true;
    std::string version = "2.0";

    std::string Json() const {
        std::vector<u8> data;
        auto push = [&](const void* p, usize n) { const u8* b = static_cast<const u8*>(p); data.insert(data.end(), b, b + n); };
        const usize position_bytes = positions.size() * 4, normal_bytes = normals.size() * 4, index_bytes = indices.size() * 2;
        push(positions.data(), position_bytes);
        push(normals.data(), normal_bytes);
        push(indices.data(), index_bytes);
        const std::string uri = "data:application/octet-stream;base64," + Base64(data);
        std::string attributes;
        if (with_position_attribute) attributes += "\"POSITION\":0";
        if (with_normals) attributes += std::string(attributes.empty() ? "" : ",") + "\"NORMAL\":1";
        return "{\"asset\":{\"version\":\"" + version + "\"},\"buffers\":[{\"byteLength\":" + std::to_string(data.size()) + ",\"uri\":\"" + uri + "\"}],"
               "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(position_bytes) + "},{\"buffer\":0,\"byteOffset\":" + std::to_string(position_bytes) +
               ",\"byteLength\":" + std::to_string(normal_bytes) + "},{\"buffer\":0,\"byteOffset\":" + std::to_string(position_bytes + normal_bytes) + ",\"byteLength\":" + std::to_string(index_bytes) + "}],"
               "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(positions.size() / 3) + ",\"type\":\"VEC3\"},"
               "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(normals.size() / 3) + ",\"type\":\"VEC3\"},"
               "{\"bufferView\":2,\"componentType\":5123,\"count\":" + std::to_string(indices.size()) + ",\"type\":\"SCALAR\"}],"
               "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{" + attributes + "},\"indices\":2,\"mode\":" + std::to_string(mode) + "}]}]}";
    }
};

ImportResult ImportText(const std::string& text) {
    return ModelImporter().ImportFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), ModelImporter().DefaultSettings());
}

stdfs::path Scratch() {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_import_diagnostics";
    stdfs::create_directories(dir);
    return dir;
}
int g_counter = 0;

ImportResult ImportModelFile(const std::string& text) {
    const stdfs::path file = Scratch() / ("model" + std::to_string(g_counter++) + ".gltf");
    { std::ofstream(file, std::ios::binary) << text; }
    AssetRecord record;
    record.path = "models/" + file.filename().string();
    return ModelImporter().Import({record, file, ModelImporter().DefaultSettings()});
}

ImportResult ImportTextureBytes(const std::string& bytes) {
    const stdfs::path file = Scratch() / ("texture" + std::to_string(g_counter++) + ".png");
    { std::ofstream(file, std::ios::binary) << bytes; }
    AssetRecord record;
    record.path = "textures/" + file.filename().string();
    return TextureImporter().Import({record, file, TextureImporter().DefaultSettings()});
}

} // namespace

AETHER_TEST(ImportDiagnostics_AGoodModelSaysNothing) {
    const ImportResult good = ImportText(Triangle().Json());
    AETHER_CHECK(good.ok && good.error.empty());
    AETHER_CHECK(good.warnings.empty());
    for (const auto& entry : stdfs::directory_iterator(stdfs::path(AETHER_REPO_ASSETS_DIR) / "models")) { // and the repo's own models
        if (entry.path().extension() != ".gltf" && entry.path().extension() != ".glb") continue;
        AssetRecord record;
        record.path = "models/" + entry.path().filename().string();
        const ImportResult r = ModelImporter().Import({record, entry.path(), ModelImporter().DefaultSettings()});
        if (!r.ok || !r.warnings.empty()) std::printf("    %s: %s %s\n", entry.path().filename().string().c_str(), r.error.c_str(), r.warnings.empty() ? "" : r.warnings[0].c_str());
        AETHER_CHECK(r.ok && r.warnings.empty());
    }
}

AETHER_TEST(ImportDiagnostics_ModelFilesThatAreNotGltfSayWhatIsWrongAndWhere) {
    const ImportResult empty = ImportModelFile("");
    AETHER_CHECK(!empty.ok && Has(empty.error, "is empty"));
    const ImportResult text = ImportModelFile("hello");
    AETHER_CHECK(!text.ok && Has(text.error, "not valid JSON") && Has(text.error, "line 1, column 1")); // where the parser gave up
    const ImportResult cut = ImportModelFile("{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primi");
    AETHER_CHECK(!cut.ok && Has(cut.error, "not valid JSON") && Has(cut.error, "column"));
    const ImportResult other_json = ImportModelFile("{\"name\":\"a package.json, say\"}");
    AETHER_CHECK(!other_json.ok && Has(other_json.error, "no \"asset\" object") && Has(other_json.error, "different JSON file"));
    const ImportResult top_array = ImportModelFile("[1,2,3]");
    AETHER_CHECK(!top_array.ok && Has(top_array.error, "not a JSON object"));
    Triangle old;
    old.version = "1.0";
    const ImportResult v1 = ImportModelFile(old.Json());
    AETHER_CHECK(!v1.ok && Has(v1.error, "glTF 1.0") && Has(v1.error, "re-export"));
    const ImportResult no_version = ImportModelFile("{\"asset\":{}}");
    AETHER_CHECK(!no_version.ok && Has(no_version.error, "no asset.version"));
    // The memory path (tools and tests) gives the same reasons.
    const ImportResult memory = ImportText("hello");
    AETHER_CHECK(!memory.ok && Has(memory.error, "not valid JSON") && Has(memory.error, "column"));
    AETHER_CHECK(!ImportText("").ok && Has(ImportText("").error, "empty"));
}

AETHER_TEST(ImportDiagnostics_BufferFailuresNameTheFileAndTheFix) {
    const ImportResult missing = ImportModelFile("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"meshes.bin\",\"byteLength\":36}]}");
    AETHER_CHECK(!missing.ok && Has(missing.error, "meshes.bin") && Has(missing.error, "missing") && Has(missing.error, "copy it next to the model"));
    const ImportResult no_comma = ImportModelFile("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"data:application/octet-stream;base64\",\"byteLength\":3}]}");
    AETHER_CHECK(!no_comma.ok && Has(no_comma.error, "no comma"));
    const ImportResult external_in_memory = ImportText("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"meshes.bin\",\"byteLength\":36}]}");
    AETHER_CHECK(!external_in_memory.ok && Has(external_in_memory.error, "external file") && Has(external_in_memory.error, "embed it"));
}

AETHER_TEST(ImportDiagnostics_AModelThatImportsAsNothingSaysSoAndWhy) {
    const ImportResult none = ImportModelFile("{\"asset\":{\"version\":\"2.0\"}}");
    AETHER_CHECK(none.ok && AnyWarning(none, "contains no meshes"));
    const ImportResult empty_mesh = ImportModelFile("{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"name\":\"Rock\",\"primitives\":[]}]}");
    AETHER_CHECK(empty_mesh.ok && AnyWarning(empty_mesh, "mesh 0 \"Rock\" has an empty") && AnyWarning(empty_mesh, "not one drawable primitive"));
    Triangle lines;
    lines.mode = 1;
    const ImportResult lines_result = ImportText(lines.Json());
    AETHER_CHECK(lines_result.ok && AnyWarning(lines_result, "mesh 0 \"Tri\" primitive 0 skipped: draw mode 1 is not supported") && AnyWarning(lines_result, "not one drawable primitive"));
    Triangle no_position;
    no_position.with_position_attribute = false;
    const ImportResult no_position_result = ImportText(no_position.Json());
    AETHER_CHECK(AnyWarning(no_position_result, "no POSITION attribute"));
    // An accessor that reads past its data: which one, and by how much.
    Triangle big;
    std::string text = big.Json();
    const usize count_at = text.find("\"count\":3");
    AETHER_CHECK(count_at != std::string::npos);
    text.replace(count_at, 9, "\"count\":999");
    const ImportResult past = ImportText(text);
    AETHER_CHECK(past.ok && AnyWarning(past, "primitive 0 skipped: POSITION unusable") && AnyWarning(past, "accessor 0 reads past the end of its bufferView") && AnyWarning(past, "999 elements"));
}

AETHER_TEST(ImportDiagnostics_AnAttributeThatIsDroppedIsReported) {
    Triangle no_normals;
    no_normals.with_normals = false;
    AETHER_CHECK(AnyWarning(ImportText(no_normals.Json()), "no NORMAL attribute"));
    Triangle short_normals;
    short_normals.normals = {0, 0, 1, 0, 0, 1}; // two normals for three positions
    const ImportResult r = ImportText(short_normals.Json());
    AETHER_CHECK(r.ok && AnyWarning(r, "NORMAL ignored") && AnyWarning(r, "2 entries for 3 positions"));
}

AETHER_TEST(ImportDiagnostics_CorruptGeometryIsRefusedOrFlagged) {
    Triangle bad_index;
    bad_index.indices = {0, 1, 7}; // a vertex that does not exist
    const ImportResult refused = ImportText(bad_index.Json());
    AETHER_CHECK(!refused.ok && Has(refused.error, "mesh 0 primitive 0") && Has(refused.error, "index(es) past its 3 vertices") && Has(refused.error, "the first is 7"));
    Triangle nan;
    nan.positions[4] = std::nanf("");
    const ImportResult nan_result = ImportText(nan.Json());
    AETHER_CHECK(nan_result.ok && AnyWarning(nan_result, "1 vertex position(s) that are NaN or infinite"));
    Triangle ragged;
    ragged.indices = {0, 1, 2, 0};
    const ImportResult ragged_result = ImportText(ragged.Json());
    AETHER_CHECK(ragged_result.ok && AnyWarning(ragged_result, "4 indices, not a multiple of three"));
}

AETHER_TEST(ImportDiagnostics_TexturesSayWhatTheFileIsAndWhatToDo) {
    const ImportResult empty = ImportTextureBytes("");
    AETHER_CHECK(!empty.ok && Has(empty.error, "empty") && Has(empty.error, "0 bytes"));
    const ImportResult lfs = ImportTextureBytes("version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 12345\n");
    AETHER_CHECK(!lfs.ok && Has(lfs.error, "Git LFS pointer") && Has(lfs.error, "git lfs pull"));
    const ImportResult html = ImportTextureBytes("<!DOCTYPE html><html><body>404 Not Found</body></html>");
    AETHER_CHECK(!html.ok && Has(html.error, "web page") && Has(html.error, "failed download"));
    const ImportResult webp = ImportTextureBytes(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16));
    AETHER_CHECK(!webp.ok && Has(webp.error, "WebP format") && Has(webp.error, "convert it to PNG"));
    const ImportResult text = ImportTextureBytes("hello world, I am a text file renamed to .png");
    AETHER_CHECK(!text.ok && Has(text.error, "text file, not an image") && Has(text.error, "hello world"));
    const ImportResult noise = ImportTextureBytes(std::string("\x01\x02\x03\xFE\xFD\xA0\x00\x10\x11", 9));
    AETHER_CHECK(!noise.ok && Has(noise.error, "not a recognised image") && Has(noise.error, "01 02 03 FE") && Has(noise.error, "PNG, JPEG"));
    const ImportResult header_only = ImportTextureBytes(std::string("\x89PNG\r\n\x1a\n", 8));
    AETHER_CHECK(!header_only.ok && Has(header_only.error, "truncated"));
    const ImportResult cut = ImportTextureBytes(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x02\0\0\0", 25));
    AETHER_CHECK(!cut.ok && Has(cut.error, "16 x 16 PNG") && Has(cut.error, "truncated or damaged") && Has(cut.error, "25 bytes"));
    // Too big is told apart from damaged: the size is in the header.
    const ImportResult huge = ImportTextureBytes(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\x20\0\0\0\x20\0\x08\x02\0\0\0\0\0\0\0", 33));
    AETHER_CHECK(!huge.ok && Has(huge.error, "8192 x 8192") && Has(huge.error, "over the budget") && Has(huge.error, "make it smaller"));
    const ImportResult jpeg = ImportTextureBytes(std::string("\xFF\xD8\xFF\xE0\0\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0\xFF\xC0\0\x11\x08\x01\0\x01\0\x03", 30));
    AETHER_CHECK(!jpeg.ok && Has(jpeg.error, "256 x 256 JPEG") && Has(jpeg.error, "truncated or damaged"));
    const ImportResult missing = TextureImporter().Import({AssetRecord{}, Scratch() / "no_such_file.png", TextureImporter().DefaultSettings()});
    AETHER_CHECK(!missing.ok && Has(missing.error, "missing") && Has(missing.error, "no_such_file.png"));
    // The in-memory path says the same.
    const std::string lfs_bytes = "version https://git-lfs.github.com/spec/v1\n";
    const ImportResult memory = TextureImporter().ImportFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(lfs_bytes.data()), lfs_bytes.size()), TextureImporter().DefaultSettings());
    AETHER_CHECK(!memory.ok && Has(memory.error, "Git LFS pointer"));
}

AETHER_TEST(ImportDiagnostics_AGoodTextureImportsCleanly) {
    for (const auto& entry : stdfs::directory_iterator(stdfs::path(AETHER_REPO_ASSETS_DIR) / "textures")) {
        if (entry.path().extension() != ".png" && entry.path().extension() != ".jpg") continue;
        AssetRecord record;
        record.path = "textures/" + entry.path().filename().string();
        const ImportResult r = TextureImporter().Import({record, entry.path(), TextureImporter().DefaultSettings()});
        if (!r.ok) std::printf("    %s: %s\n", entry.path().filename().string().c_str(), r.error.c_str());
        AETHER_CHECK(r.ok && r.warnings.empty());
    }
}
