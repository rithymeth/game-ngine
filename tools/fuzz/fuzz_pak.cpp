// Fuzz target: the .apak reader (Phase 47 step 1). Opens arbitrary bytes as an archive and, if it
// opens, reads every entry. It must never crash, hang or read out of bounds.
#include "fuzz_common.h"

#include "aether/pak/pak.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::pak::PakReader reader;
    std::string error;
    if (reader.OpenMemory(std::vector<aether::u8>(data, data + size), &error)) {
        for (const auto& entry : reader.Entries()) {
            std::vector<aether::u8> out;
            reader.Read(entry.path, out, &error);
        }
        (void)reader.Verify();
    }
    return 0;
}
