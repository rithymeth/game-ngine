// Fuzz target: the cooked mesh container (AMSH). `fuzz_model_import` covers
// importing a source model; this covers the container the importer emits and the
// cooker reads back (model_importer.cpp -> cooker.cpp), which also travels inside
// .apak archives, so it must survive arbitrary bytes.
#include "fuzz_common.h"

#include "aether/assets/model_importer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::MeshData mesh;
    (void)aether::assets::DecodeMeshData(std::vector<aether::u8>(data, data + size), mesh);
    return 0;
}
