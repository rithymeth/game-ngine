// Fuzz target: the save envelope (Phase 47 step 1): the checked reader used for save slots and settings, and
// the inspector the editor's Save Inspector uses, over arbitrary bytes. The first input byte picks which.
#include "fuzz_common.h"

#include "aether/reflection/serialize.h"
#include "aether/save/envelope.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fuzz_save {
struct FuzzProfile {
    aether::i32 level = 1;
    aether::f32 health = 100.0f;
    std::string name = "Hero";
    std::vector<aether::i32> inventory;
    bool hard_mode = false;
};
} // namespace fuzz_save

AETHER_REFLECT(fuzz_save::FuzzProfile, 1, AETHER_FIELD(level, Field_EditAnywhere), AETHER_FIELD(health, Field_EditAnywhere),
               AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(inventory, Field_EditAnywhere), AETHER_FIELD(hard_mode, Field_EditAnywhere))

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const std::span<const aether::u8> bytes(data + 1, size - 1);
    if (data[0] & 1) {
        (void)aether::save::envelope::InspectBytes(bytes);
    } else {
        fuzz_save::FuzzProfile profile;
        (void)aether::save::envelope::ReadFromMemory(bytes, "fuzz", "aether.save", aether::reflect::Reflect<fuzz_save::FuzzProfile>(), &profile);
    }
    return 0;
}
