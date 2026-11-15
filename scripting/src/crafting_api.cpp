#include "aether/script/crafting_api.h"

#if AETHER_KIT_CRAFTING
#include "aether/crafting/crafting_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_CRAFTING

namespace {

using namespace aether::craft;

bool NeedEntity(NativeCall& c) {
    if (c.IsEntity(0)) return true;
    c.Fail("Crafting: the owner must be an entity");
    return false;
}
bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Crafting: ") + what + " must be a string");
    return false;
}
i32 Times(NativeCall& c, usize i) { return c.IsNumber(i) ? static_cast<i32>(c.Number(i)) : 1; }

std::string Line(const std::vector<Ingredient>& lines) {
    std::string text;
    for (const Ingredient& line : lines) {
        if (!text.empty()) text += " + ";
        text += std::to_string(line.count) + " " + line.item;
    }
    return text;
}

} // namespace

void InstallCraftingApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Crafting", name, std::move(fn)); };
    def("Craft", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedString(c, 1, "the recipe")) return;
        CraftingSystem* system = CraftingSystem::Active();
        c.Return(std::string(system ? ResultName(system->Craft(c.EntityArg(0), c.String(1), Times(c, 2))) : ResultName(Result::UnknownRecipe)));
    });
    def("CanCraft", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedString(c, 1, "the recipe")) return;
        const CraftingSystem* system = CraftingSystem::Active();
        c.Return(system != nullptr && system->CanCraft(c.EntityArg(0), c.String(1), Times(c, 2)));
    });
    def("MissingCount", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedString(c, 1, "the recipe")) return;
        const CraftingSystem* system = CraftingSystem::Active();
        c.Return(static_cast<f64>(system ? system->Missing(c.EntityArg(0), c.String(1), Times(c, 2)).size() : 0));
    });
    def("Exists", [](NativeCall& c) {
        if (!NeedString(c, 0, "the recipe")) return;
        const CraftingSystem* system = CraftingSystem::Active();
        c.Return(system != nullptr && system->Recipes().Find(c.String(0)) != nullptr);
    });
    def("GetTitle", [](NativeCall& c) {
        if (!NeedString(c, 0, "the recipe")) return;
        const CraftingSystem* system = CraftingSystem::Active();
        const RecipeDef* recipe = system ? system->Recipes().Find(c.String(0)) : nullptr;
        c.Return(recipe ? (recipe->title.empty() ? recipe->name : recipe->title) : std::string());
    });
    def("Describe", [](NativeCall& c) {
        if (!NeedString(c, 0, "the recipe")) return;
        const CraftingSystem* system = CraftingSystem::Active();
        const RecipeDef* recipe = system ? system->Recipes().Find(c.String(0)) : nullptr;
        c.Return(recipe ? Line(recipe->inputs) + " -> " + Line(recipe->outputs) : std::string());
    });
}

#else

void InstallCraftingApi(LuauHost&) {}

#endif

} // namespace aether::script
