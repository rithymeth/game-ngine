// Fuzz target: the plugin descriptor loader (Phase 47 step 8) over arbitrary bytes.
#include "fuzz_common.h"

#include "aether/plugin/plugin.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::plugin::PluginDescriptor descriptor;
    std::string error;
    (void)aether::plugin::LoadPluginDescriptorFromMemory(std::span<const aether::u8>(data, size), "fuzz.aplugin", descriptor, &error);
    return 0;
}
