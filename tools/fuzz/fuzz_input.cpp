// Fuzz target: the input asset loaders (Phase 47 step 1). The first input byte picks actions or mapping contexts;
// the rest is the JSON text.
#include "fuzz_common.h"

#include "aether/input/bindings.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    aether::input::InputAssetLibrary library;
    std::string error;
    (void)library.AddFromText((data[0] & 1) ? "InputMapping" : "InputAction", "fuzz.asset", text, &error);
    return 0;
}
