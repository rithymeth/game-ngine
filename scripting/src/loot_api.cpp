#include "aether/script/loot_api.h"

#if AETHER_KIT_LOOT
#include "aether/loot/loot_system.h"

#include <cmath>
#endif

namespace aether::script {

#if AETHER_KIT_LOOT

namespace {

using namespace aether::loot;

bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Loot: ") + what + " must be a string");
    return false;
}

// A script number as a seed: any finite number, by its bits' worth (the whole part, wrapped), so 12 and 12.0 are the same seed.
u64 SeedOf(f64 n) { return std::isfinite(n) ? static_cast<u64>(static_cast<i64>(std::fmod(std::floor(n), 9.0e18))) : 0u; }

} // namespace

void InstallLootApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Loot", name, std::move(fn)); };
    def("Give", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Loot: the owner must be an entity");
            return;
        }
        if (!NeedString(c, 1, "the table")) return;
        LootSystem* system = LootSystem::Active();
        if (system == nullptr) {
            c.Return(std::string(ResultName(Result::UnknownTable)));
            return;
        }
        const Result result = c.IsNumber(2) ? system->GiveSeeded(c.EntityArg(0), c.String(1), SeedOf(c.Number(2))) : system->Give(c.EntityArg(0), c.String(1));
        c.Return(std::string(ResultName(result)));
    });
    def("LastGiven", [](NativeCall& c) {
        const LootSystem* system = LootSystem::Active();
        c.Return(static_cast<f64>(system ? system->LastGiven() : 0));
    });
    def("Roll", [](NativeCall& c) {
        if (!NeedString(c, 0, "the table")) return;
        LootSystem* system = LootSystem::Active();
        if (system == nullptr || system->Tables().Find(c.String(0)) == nullptr) {
            c.Return(std::string());
            return;
        }
        c.Return(DescribeDrops(system->Roll(c.String(0))));
    });
    def("SetSeed", [](NativeCall& c) {
        if (!c.IsNumber(0)) {
            c.Fail("Loot: the seed must be a number");
            return;
        }
        if (LootSystem* system = LootSystem::Active()) system->SetSeed(SeedOf(c.Number(0)));
    });
    def("Exists", [](NativeCall& c) {
        if (!NeedString(c, 0, "the table")) return;
        const LootSystem* system = LootSystem::Active();
        c.Return(system != nullptr && system->Tables().Find(c.String(0)) != nullptr);
    });
    def("GetTitle", [](NativeCall& c) {
        if (!NeedString(c, 0, "the table")) return;
        const LootSystem* system = LootSystem::Active();
        const LootTableDef* table = system ? system->Tables().Find(c.String(0)) : nullptr;
        c.Return(table ? (table->title.empty() ? table->name : table->title) : std::string());
    });
}

#else

void InstallLootApi(LuauHost&) {}

#endif

} // namespace aether::script
