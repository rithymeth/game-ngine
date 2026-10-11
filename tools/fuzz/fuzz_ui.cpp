// Fuzz target: the UI data loaders (Phase 47 step 1). The first input byte picks the layout, theme or animation
// loader; the rest is the text.
#include "fuzz_common.h"

#include "aether/ui/layout_file.h"
#include "aether/ui/theme.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
    std::string error;
    switch (data[0] % 3) {
    case 0: {
        aether::ui::LayoutDocument document;
        (void)aether::ui::LoadLayout(text, document, {}, &error);
        break;
    }
    case 1: {
        aether::ui::Theme theme;
        (void)aether::ui::LoadTheme(text, theme, {}, &error);
        break;
    }
    default: {
        const auto json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        aether::ui::UIAnimation animation;
        (void)aether::ui::AnimationFromJson(json, animation, &error);
        break;
    }
    }
    return 0;
}
