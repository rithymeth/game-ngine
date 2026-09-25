#pragma once

#include "aether/core/base.h"

#include <atomic>
#include <bitset>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aether {

using ComponentId = u32;
inline constexpr usize kMaxComponentTypes = 64;
inline constexpr ComponentId kInvalidComponentId = static_cast<ComponentId>(-1);
using ComponentMask = std::bitset<kMaxComponentTypes>;

// Type-erased operations an Archetype (or the scene serializer) needs to
// manage a component's storage without knowing its C++ type: how big a slot
// is, how to construct/destruct/move one in place, and how to turn one into
// bytes and back. Populated once per type the first time GetComponentId<T>()
// is called for it; serialize/deserialize default to a raw memcpy of the
// type, which is correct for POD components (Transform, Velocity, ...) but
// wrong for anything owning a resource or a live runtime handle (a component
// wrapping a physics engine body ID, say) — such types must override their
// serializer with SetComponentSerializer() to (de)serialize the meaningful
// data instead of the handle itself.
struct ComponentInfo {
    usize size = 0;
    usize alignment = 0;
    const char* name = "";
    void (*construct)(void* dst) = nullptr;
    void (*destruct)(void* ptr) = nullptr;
    void (*move)(void* dst, void* src) = nullptr;
    void (*serialize)(const void* component, std::vector<u8>& out) = nullptr;
    void (*deserialize)(void* component, const u8* data, usize size) = nullptr;
};

namespace detail {

inline std::atomic<ComponentId>& ComponentIdCounter() {
    static std::atomic<ComponentId> counter{0};
    return counter;
}

inline ComponentInfo* ComponentRegistry() {
    static ComponentInfo registry[kMaxComponentTypes];
    return registry;
}

// Maps a component's display name to its runtime-assigned id, so a scene
// file can identify components by name (stable across process restarts and
// registration-order differences) instead of the numeric id, which is only
// meaningful within the process that assigned it.
inline std::unordered_map<std::string, ComponentId>& ComponentNameRegistry() {
    static std::unordered_map<std::string, ComponentId> registry;
    return registry;
}

} // namespace detail

template <typename T>
ComponentId GetComponentId() {
    static const ComponentId id = [] {
        ComponentId new_id = detail::ComponentIdCounter().fetch_add(1, std::memory_order_relaxed);
        AETHER_ASSERT(new_id < kMaxComponentTypes);

        ComponentInfo& info = detail::ComponentRegistry()[new_id];
        info.size = sizeof(T);
        info.alignment = alignof(T);
        info.name = typeid(T).name();
        info.construct = [](void* dst) { new (dst) T(); };
        info.destruct = [](void* ptr) { static_cast<T*>(ptr)->~T(); };
        info.move = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
        info.serialize = [](const void* component, std::vector<u8>& out) {
            const u8* bytes = static_cast<const u8*>(component);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        };
        info.deserialize = [](void* component, const u8* data, usize size) {
            AETHER_ASSERT(size == sizeof(T));
            std::memcpy(component, data, size < sizeof(T) ? size : sizeof(T));
        };
        detail::ComponentNameRegistry()[info.name] = new_id;
        return new_id;
    }();
    return id;
}

inline const ComponentInfo& GetComponentInfo(ComponentId id) {
    return detail::ComponentRegistry()[id];
}

inline ComponentId FindComponentIdByName(const std::string& name) {
    auto& registry = detail::ComponentNameRegistry();
    auto it = registry.find(name);
    return (it != registry.end()) ? it->second : kInvalidComponentId;
}

// Overrides the (de)serialize functions and display name registered for T —
// for a component like a physics body handle, where the default raw-byte
// copy would serialize a runtime-only handle instead of the data needed to
// reconstruct it. Safe to call regardless of whether GetComponentId<T>() has
// run yet; this ensures it has.
template <typename T>
void SetComponentSerializer(const char* name, void (*serialize)(const void*, std::vector<u8>&),
                             void (*deserialize)(void*, const u8*, usize)) {
    ComponentId id = GetComponentId<T>();
    ComponentInfo& info = detail::ComponentRegistry()[id];
    info.name = name;
    info.serialize = serialize;
    info.deserialize = deserialize;
    detail::ComponentNameRegistry()[name] = id;
}

// Small helpers for writing a custom (de)serializer field-by-field, for
// components that need to skip or transform some fields (a live runtime
// handle, say) rather than serialize the whole struct as raw bytes.
template <typename T>
void AppendComponentField(std::vector<u8>& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "AppendComponentField requires a trivially copyable type");
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
void ReadComponentField(const u8* data, usize size, usize& offset, T& out_value) {
    static_assert(std::is_trivially_copyable_v<T>, "ReadComponentField requires a trivially copyable type");
    AETHER_ASSERT(offset + sizeof(T) <= size);
    std::memcpy(&out_value, data + offset, sizeof(T));
    offset += sizeof(T);
}

template <typename... Ts>
ComponentMask ComponentMaskOf() {
    ComponentMask mask;
    (mask.set(GetComponentId<Ts>()), ...);
    return mask;
}

} // namespace aether
