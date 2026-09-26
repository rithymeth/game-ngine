#include "aether/reflection/registry.h"

#include "aether/core/log.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace aether::reflect {

namespace {

struct RegistryState {
    std::mutex mutex;
    std::unordered_map<TypeId, const TypeInfo*> by_id;
    std::vector<const TypeInfo*> in_order;
};

// Function-local static so registration from other translation units' static
// initializers is safe regardless of initialization order.
RegistryState& State() {
    static RegistryState state;
    return state;
}

} // namespace

void TypeRegistry::Register(const TypeInfo& info) {
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto [it, inserted] = state.by_id.emplace(info.id, &info);
    if (inserted) {
        state.in_order.push_back(&info);
        return;
    }
    if (it->second != &info) {
        AETHER_LOG_ERROR("Reflection",
                         "Two different types share the declared name \"%s\" (TypeId %llu); keeping the first. "
                         "Rename one of them.",
                         info.name, static_cast<unsigned long long>(info.id));
    }
}

const TypeInfo* TypeRegistry::Find(TypeId id) {
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.by_id.find(id);
    return it != state.by_id.end() ? it->second : nullptr;
}

const TypeInfo* TypeRegistry::Find(std::string_view name) {
    const TypeInfo* info = Find(HashTypeName(name));
    // Guard against a (vanishingly unlikely) hash collision with a different name.
    return (info != nullptr && name == info->name) ? info : nullptr;
}

std::vector<const TypeInfo*> TypeRegistry::AllTypes() {
    RegistryState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.in_order;
}

} // namespace aether::reflect
