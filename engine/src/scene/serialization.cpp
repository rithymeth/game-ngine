#include "aether/scene/serialization.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace aether {

namespace {

constexpr char kMagic[4] = {'A', 'E', 'S', 'C'};
// v1: per entity a u64 component mask, then {name, bytes} per set bit.
// v2: per entity a u32 component count, then {name, u8 encoding, bytes}.
constexpr u32 kLegacyVersion = 1;
constexpr u32 kVersion = 2;
constexpr u32 kJsonSceneVersion = 1;

void AppendU32(std::vector<u8>& buffer, u32 value) {
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
}

void AppendBytes(std::vector<u8>& buffer, const void* data, usize size) {
    const u8* bytes = static_cast<const u8*>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
}

// Cursor over an in-memory buffer for reading LoadScene's binary format.
class Reader {
public:
    explicit Reader(const std::vector<u8>& buffer) : buffer_(buffer) {}

    bool ReadBytes(void* dst, usize n) {
        if (offset_ + n > buffer_.size()) {
            return false;
        }
        std::memcpy(dst, buffer_.data() + offset_, n);
        offset_ += n;
        return true;
    }

    bool ReadU8(u8& out) { return ReadBytes(&out, sizeof(out)); }
    bool ReadU32(u32& out) { return ReadBytes(&out, sizeof(out)); }
    bool ReadU64(u64& out) { return ReadBytes(&out, sizeof(out)); }

    bool ReadString(std::string& out, u32 length) {
        out.resize(length);
        return length == 0 || ReadBytes(out.data(), length);
    }

private:
    const std::vector<u8>& buffer_;
    usize offset_ = 0;
};

// One live entity, located for type-erased component access.
struct EntitySlot {
    Entity entity;
    const Archetype* archetype;
    usize chunk;
    u32 row;
};

// Every entity in `world`, in entity-index order (the archetype map's own
// iteration order depends on hashing and insertion history, so it isn't
// stable enough for reproducible files).
std::vector<EntitySlot> CollectEntities(const World& world) {
    std::vector<EntitySlot> slots;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                slots.push_back({entities[row], &archetype, c, row});
            }
        }
    });
    std::sort(slots.begin(), slots.end(),
              [](const EntitySlot& a, const EntitySlot& b) { return a.entity.index < b.entity.index; });
    return slots;
}

std::vector<ComponentId> ComponentIdsOf(const ComponentMask& mask) {
    std::vector<ComponentId> ids;
    for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
        if (mask.test(id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

const void* ComponentPtr(const EntitySlot& slot, ComponentId id) {
    const u8* base = static_cast<const u8*>(slot.archetype->ComponentArray(slot.chunk, id));
    return base + static_cast<usize>(slot.row) * GetComponentInfo(id).size;
}

struct PendingComponent {
    ComponentId id;
    ComponentEncoding encoding;
    std::vector<u8> data;
};

// Deserializes one component record, bridging encodings when a component's
// representation changed since the file was written.
void ApplyComponent(void* dst, const ComponentInfo& info, const PendingComponent& pending, const std::string& path) {
    if (pending.encoding == info.encoding) {
        if (info.deserialize) {
            info.deserialize(dst, pending.data.data(), pending.data.size());
        }
        return;
    }
    if (pending.encoding == ComponentEncoding::Raw && info.encoding == ComponentEncoding::Reflected) {
        // Saved as raw bytes before the type was reflected: still valid if the
        // layout is unchanged and the type is plain data.
        if (info.trivially_copyable && pending.data.size() == info.size) {
            std::memcpy(dst, pending.data.data(), info.size);
            return;
        }
        AETHER_LOG_WARN("Scene",
                        "Component '%s' in %s was saved as raw bytes by an older build and its layout changed; "
                        "loaded with default values",
                        info.name, path.c_str());
        return;
    }
    AETHER_LOG_WARN("Scene", "Component '%s' in %s was saved in an incompatible encoding; loaded with default values",
                    info.name, path.c_str());
}

} // namespace

bool SaveScene(const World& world, const std::string& path) {
    std::vector<u8> buffer;
    AppendBytes(buffer, kMagic, sizeof(kMagic));
    AppendU32(buffer, kVersion);

    std::vector<EntitySlot> slots = CollectEntities(world);
    AppendU32(buffer, static_cast<u32>(slots.size()));

    std::vector<u8> component_bytes;
    for (const EntitySlot& slot : slots) {
        std::vector<ComponentId> ids = ComponentIdsOf(slot.archetype->Mask());
        AppendU32(buffer, static_cast<u32>(ids.size()));
        for (ComponentId id : ids) {
            const ComponentInfo& info = GetComponentInfo(id);
            std::string name = info.name;
            AppendU32(buffer, static_cast<u32>(name.size()));
            AppendBytes(buffer, name.data(), name.size());
            buffer.push_back(static_cast<u8>(info.encoding));

            component_bytes.clear();
            if (info.serialize) {
                info.serialize(ComponentPtr(slot, id), component_bytes);
            }
            AppendU32(buffer, static_cast<u32>(component_bytes.size()));
            AppendBytes(buffer, component_bytes.data(), component_bytes.size());
        }
    }

    if (!fs::WriteFileBytes(path, buffer.data(), buffer.size())) {
        AETHER_LOG_ERROR("Scene", "Failed to write scene file: %s", path.c_str());
        return false;
    }
    AETHER_LOG_INFO("Scene", "Saved %zu entities to %s", slots.size(), path.c_str());
    return true;
}

bool LoadScene(World& world, const std::string& path) {
    std::vector<u8> buffer;
    if (!fs::ReadFileBytes(path, buffer)) {
        AETHER_LOG_ERROR("Scene", "Failed to read scene file: %s", path.c_str());
        return false;
    }

    Reader reader(buffer);
    char magic[4];
    u32 version = 0;
    if (!reader.ReadBytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        AETHER_LOG_ERROR("Scene", "Not an Aether scene file: %s", path.c_str());
        return false;
    }
    if (!reader.ReadU32(version) || (version != kVersion && version != kLegacyVersion)) {
        AETHER_LOG_ERROR("Scene", "Unsupported scene version in %s", path.c_str());
        return false;
    }

    u32 entity_count = 0;
    if (!reader.ReadU32(entity_count)) {
        AETHER_LOG_ERROR("Scene", "Truncated scene file: %s", path.c_str());
        return false;
    }

    u32 loaded = 0;
    for (u32 e = 0; e < entity_count; ++e) {
        u32 component_count = 0;
        if (version == kLegacyVersion) {
            // v1 wrote the entity's component mask; one record follows per set bit.
            u64 raw_mask = 0;
            if (!reader.ReadU64(raw_mask)) {
                break;
            }
            component_count = static_cast<u32>(std::popcount(raw_mask));
        } else if (!reader.ReadU32(component_count)) {
            break;
        }

        std::vector<PendingComponent> pending;
        ComponentMask resolved_mask;
        for (u32 c = 0; c < component_count; ++c) {
            u32 name_len = 0;
            std::string name;
            u8 encoding = static_cast<u8>(ComponentEncoding::Raw);
            u32 data_len = 0;
            std::vector<u8> data;
            if (!reader.ReadU32(name_len) || !reader.ReadString(name, name_len) ||
                (version != kLegacyVersion && !reader.ReadU8(encoding)) || !reader.ReadU32(data_len)) {
                AETHER_LOG_ERROR("Scene", "Truncated scene file: %s", path.c_str());
                return false;
            }
            data.resize(data_len);
            if (data_len > 0 && !reader.ReadBytes(data.data(), data_len)) {
                AETHER_LOG_ERROR("Scene", "Truncated scene file: %s", path.c_str());
                return false;
            }

            ComponentId resolved_id = FindComponentIdByName(name);
            if (resolved_id == kInvalidComponentId) {
                AETHER_LOG_WARN("Scene", "Skipping unknown component '%s' while loading %s", name.c_str(),
                                path.c_str());
                continue;
            }
            ComponentEncoding file_encoding = static_cast<ComponentEncoding>(encoding);
            if (version == kLegacyVersion) {
                // v1 didn't record encodings: components with a custom
                // serializer used it; everything else was raw bytes.
                file_encoding = GetComponentInfo(resolved_id).encoding == ComponentEncoding::Custom
                                    ? ComponentEncoding::Custom
                                    : ComponentEncoding::Raw;
            }
            resolved_mask.set(resolved_id);
            pending.push_back({resolved_id, file_encoding, std::move(data)});
        }

        Entity entity = world.CreateEntityRaw(resolved_mask);
        for (const PendingComponent& component : pending) {
            void* ptr = world.GetComponentRaw(entity, component.id);
            if (ptr) {
                ApplyComponent(ptr, GetComponentInfo(component.id), component, path);
            }
        }
        ++loaded;
    }

    AETHER_LOG_INFO("Scene", "Loaded %u entities from %s", loaded, path.c_str());
    return true;
}

bool SaveSceneJson(const World& world, const std::string& path) {
    using reflect::Json;
    Json entities = Json::array();
    const ComponentId id_component = GetComponentId<IdComponent>();
    for (const EntitySlot& slot : CollectEntities(world)) {
        Json components = Json::object();
        Json guid;
        for (ComponentId id : ComponentIdsOf(slot.archetype->Mask())) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (id == id_component) {
                // The entity's identity goes at entity level (§A.3), not
                // among its components.
                guid = ToString(static_cast<const IdComponent*>(ComponentPtr(slot, id))->guid);
                continue;
            }
            if (info.reflected == nullptr) {
                AETHER_LOG_WARN("Scene", "Component '%s' isn't reflected, so it can't be saved to JSON (%s)",
                                info.name, path.c_str());
                continue;
            }
            // The reflected (declared) name, even if a custom serializer
            // renamed the component for the binary format.
            components[info.reflected->name] = reflect::ToJson(*info.reflected, ComponentPtr(slot, id));
        }
        if (!components.empty() || !guid.is_null()) {
            Json entity{{"components", std::move(components)}};
            if (!guid.is_null()) {
                entity["guid"] = std::move(guid);
            }
            entities.push_back(std::move(entity));
        }
    }

    Json scene = {{"$type", "Scene"}, {"$version", kJsonSceneVersion}, {"entities", std::move(entities)}};
    std::string text = scene.dump(2);
    text.push_back('\n');
    if (!fs::WriteFileBytes(path, text.data(), text.size())) {
        AETHER_LOG_ERROR("Scene", "Failed to write scene file: %s", path.c_str());
        return false;
    }
    AETHER_LOG_INFO("Scene", "Saved %zu entities to %s", scene["entities"].size(), path.c_str());
    return true;
}

bool LoadSceneJson(World& world, const std::string& path) {
    using reflect::Json;
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(path, bytes)) {
        AETHER_LOG_ERROR("Scene", "Failed to read scene file: %s", path.c_str());
        return false;
    }
    Json scene = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (scene.is_discarded() || !scene.is_object() || scene.value("$type", "") != "Scene") {
        AETHER_LOG_ERROR("Scene", "Not an Aether JSON scene file: %s", path.c_str());
        return false;
    }
    if (scene.value("$version", 0) != static_cast<int>(kJsonSceneVersion)) {
        AETHER_LOG_ERROR("Scene", "Unsupported JSON scene version in %s", path.c_str());
        return false;
    }
    auto entities = scene.find("entities");
    if (entities == scene.end() || !entities->is_array()) {
        AETHER_LOG_ERROR("Scene", "JSON scene has no \"entities\" array: %s", path.c_str());
        return false;
    }

    u32 loaded = 0;
    for (const Json& entity_json : *entities) {
        auto components = entity_json.find("components");
        if (!entity_json.is_object() || components == entity_json.end() || !components->is_object()) {
            AETHER_LOG_WARN("Scene", "Skipping malformed entity entry in %s", path.c_str());
            continue;
        }

        ComponentMask mask;
        std::vector<std::pair<ComponentId, const Json*>> pending;
        EntityGuid guid;
        if (auto guid_json = entity_json.find("guid"); guid_json != entity_json.end()) {
            if (guid_json->is_string() && ParseEntityGuid(guid_json->get_ref<const std::string&>(), guid)) {
                mask.set(GetComponentId<IdComponent>());
            } else {
                AETHER_LOG_WARN("Scene", "Ignoring invalid entity guid in %s", path.c_str());
            }
        }
        for (auto it = components->begin(); it != components->end(); ++it) {
            ComponentId id = FindComponentIdByName(it.key());
            if (id == kInvalidComponentId || GetComponentInfo(id).reflected == nullptr) {
                AETHER_LOG_WARN("Scene", "Skipping unknown or unreflected component '%s' while loading %s",
                                it.key().c_str(), path.c_str());
                continue;
            }
            mask.set(id);
            pending.push_back({id, &it.value()});
        }

        Entity entity = world.CreateEntityRaw(mask);
        if (!guid.IsNull()) {
            world.GetComponent<IdComponent>(entity)->guid = guid;
        }
        for (auto& [id, data] : pending) {
            reflect::FromJson(*GetComponentInfo(id).reflected, world.GetComponentRaw(entity, id), *data);
        }
        ++loaded;
    }

    AETHER_LOG_INFO("Scene", "Loaded %u entities from %s", loaded, path.c_str());
    return true;
}

} // namespace aether
