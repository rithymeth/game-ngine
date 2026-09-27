#pragma once

#include "aether/ui/animation.h"
#include "aether/ui/binding.h"
#include "aether/ui/theme.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::ui {

// `.aui` layout files (Phase 18 step 3, §18.3): a widget tree as JSON, with
// each widget's common settings (name, class, tooltip, visibility, ...),
// the slot fields that differ from the defaults, its own properties, its
// children, and data bindings.
//
// Widget types are looked up by name. The built-in ones are registered; a
// game adds its own with RegisterWidgetType.
struct WidgetType {
    std::string name;
    std::function<std::unique_ptr<Widget>()> make;
    std::function<void(const Widget&, nlohmann::json&)> save;                         // its own properties
    std::function<bool(Widget&, const nlohmann::json&, const TextureResolver&, std::string*)> load;
};
void RegisterWidgetType(WidgetType type);
const WidgetType* FindWidgetType(const std::string& name);
std::vector<std::string> WidgetTypeNames();

nlohmann::json WidgetToJson(const Widget& widget);
std::unique_ptr<Widget> WidgetFromJson(const nlohmann::json& j, const TextureResolver& textures = {}, std::string* error = nullptr);

struct LayoutDocument {
    std::unique_ptr<Widget> root;
    std::vector<Binding> bindings;
    std::vector<UIAnimation> animations; // (step 4)
};
std::string SaveLayout(const Widget& root, const std::vector<Binding>& bindings = {}, const std::vector<UIAnimation>& animations = {});
bool LoadLayout(const std::string& text, LayoutDocument& out, const TextureResolver& textures = {}, std::string* error = nullptr);

} // namespace aether::ui
