#pragma once

#include "aether/core/base.h"
#include "aether/reflection/bytes.h"
#include "aether/reflection/type_info.h"

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
inline constexpr usize kMaxComponentTypes = 256;
inline constexpr ComponentId kInvalidComponentId = static_cast<ComponentId>(-1);
using ComponentMask = std::bitset<kMaxComponentTypes>;

// How a component's serialize/deserialize functions encode it. Stored per
// component in scene files, so a scene saved before a component became
// reflected can still load after it did.
enum class ComponentEncoding : u8 {
    Raw = 0,       // default for unreflected components: the struct's bytes
    Custom = 1,    // installed by SetComponentSerializer
    Reflected = 2, // default for reflected components: reflect::AppendBinary/ReadBinary
};

// Type-erased operations an Archetype (or the scene serializer) needs to
// manage a component's storage without knowing its C++ type: how big a slot
// is, how to construct/destruct/move one in place, and how to turn one into
// bytes and back. Populated once per type the first time GetComponentId<T>()
// is called for it.
//
// Serialization defaults depend on whether the type is reflected:
// - Reflected (AETHER_REFLECT visible where GetComponentId<T>() is first
//   instantiated — i.e. in the header next to the type): the component is
//   named by its declared name and (de)serialized field by field through the
//   reflection binary archive, which tolerates added/removed fields.
// - Not reflected: named by typeid(T).name() and saved as raw bytes, which is
//   correct for POD components but compiler-specific in its naming and wrong
//   for anything owning a resource or a live runtime handle — such types
//   should be reflected, or override their serializer with
//   SetComponentSerializer().
// A reflected component's typeid name is also registered as an alias, so
// scene files saved before the type was reflected still resolve it.
struct ComponentInfo {
    usize size = 0;
    usize alignment = 0;
    const char* name = "";
    ComponentEncoding encoding = ComponentEncoding::Raw;
    const reflect::TypeInfo* reflected = nullptr; // non-null when the type is reflected
    bool trivially_copyable = false;              // raw-byte loading is only safe when true
    void (*construct)(void* dst) = nullptr;
    void (*destruct)(void* ptr) = nullptr;
    void (*move)(void* dst, void* src) = nullptr;
    void (*serialize)(const void* component, std::vector<u8>& out) = nullptr;
    void (*deserialize)(void* component, const u8* data, usize size) = nullptr;
    // Optional: loads raw bytes saved by an older build with a different
    // layout (the scene loader calls it for Raw-encoded data that the current
    // encoding can't take). Returns false to fall back to default values.
    bool (*legacy_raw_load)(void* component, const u8* data, usize size) = nullptr;
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
        info.trivially_copyable = std::is_trivially_copyable_v<T>;
        info.construct = [](void* dst) { new (dst) T(); };
        info.destruct = [](void* ptr) { static_cast<T*>(ptr)->~T(); };
        info.move = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
        if constexpr (reflect::Reflected<T>) {
            // Legacy alias: the name unreflected versions of this type were saved under.
            detail::ComponentNameRegistry()[info.name] = new_id;
            info.name = reflect::Reflect<T>().name;
            info.reflected = &reflect::Reflect<T>();
            info.encoding = ComponentEncoding::Reflected;
            info.serialize = [](const void* component, std::vector<u8>& out) {
                reflect::AppendBinary(reflect::Reflect<T>(), component, out);
            };
            info.deserialize = [](void* component, const u8* data, usize size) {
                reflect::ReadBinary(reflect::Reflect<T>(), component, data, size);
            };
        } else {
            info.serialize = [](const void* component, std::vector<u8>& out) {
                const u8* bytes = static_cast<const u8*>(component);
                out.insert(out.end(), bytes, bytes + sizeof(T));
            };
            info.deserialize = [](void* component, const u8* data, usize size) {
                AETHER_ASSERT(size == sizeof(T));
                std::memcpy(component, data, size < sizeof(T) ? size : sizeof(T));
            };
        }
        detail::ComponentNameRegistry()[info.name] = new_id;
        return new_id;
    }();
    return id;
}

// Number of component types registered so far; valid ids are [0, count).
inline ComponentId RegisteredComponentCount() {
    return detail::ComponentIdCounter().load(std::memory_order_relaxed);
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
    info.encoding = ComponentEncoding::Custom;
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

// Installs T's legacy raw loader (see ComponentInfo::legacy_raw_load). Returns
// true so it can initialize a static.
template <typename T>
bool SetLegacyRawLoader(bool (*loader)(void* component, const u8* data, usize size)) {
    detail::ComponentRegistry()[GetComponentId<T>()].legacy_raw_load = loader;
    return true;
}

// Makes `name` resolve to T when loading scenes — for files written under a
// name T no longer has (e.g. a type that moved namespaces, which changes its
// typeid name).
template <typename T>
void RegisterComponentAlias(const std::string& name) {
    detail::ComponentNameRegistry()[name] = GetComponentId<T>();
}

template <typename... Ts>
ComponentMask ComponentMaskOf() {
    ComponentMask mask;
    (mask.set(GetComponentId<Ts>()), ...);
    return mask;
}

} // namespace aether
