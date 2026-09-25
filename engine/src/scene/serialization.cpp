#include "aether/scene/serialization.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"

#include <cstring>

namespace aether {

namespace {

constexpr char kMagic[4] = {'A', 'E', 'S', 'C'};
constexpr u32 kVersion = 1;

void AppendU32(std::vector<u8>& buffer, u32 value) {
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(value));
}

void AppendU64(std::vector<u8>& buffer, u64 value) {
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

} // namespace

bool SaveScene(const World& world, const std::string& path) {
    std::vector<u8> buffer;
    AppendBytes(buffer, kMagic, sizeof(kMagic));
    AppendU32(buffer, kVersion);

    std::vector<u8> entity_section;
    u32 entity_count = 0;

    world.ForEachArchetype([&](const Archetype& archetype) {
        const ComponentMask& mask = archetype.Mask();
        std::vector<ComponentId> ids;
        for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
            if (mask.test(id)) {
                ids.push_back(id);
            }
        }

        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            u32 count = archetype.ChunkEntityCount(c);
            if (count == 0) {
                continue;
            }

            for (u32 row = 0; row < count; ++row) {
                AppendU64(entity_section, mask.to_ullong());

                for (ComponentId id : ids) {
                    const ComponentInfo& info = GetComponentInfo(id);
                    const u8* base = static_cast<const u8*>(archetype.ComponentArray(c, id));
                    const void* component_ptr = base + static_cast<usize>(row) * info.size;

                    std::string name = info.name;
                    AppendU32(entity_section, static_cast<u32>(name.size()));
                    AppendBytes(entity_section, name.data(), name.size());

                    std::vector<u8> component_bytes;
                    if (info.serialize) {
                        info.serialize(component_ptr, component_bytes);
                    }
                    AppendU32(entity_section, static_cast<u32>(component_bytes.size()));
                    AppendBytes(entity_section, component_bytes.data(), component_bytes.size());
                }
                ++entity_count;
            }
        }
    });

    AppendU32(buffer, entity_count);
    buffer.insert(buffer.end(), entity_section.begin(), entity_section.end());

    if (!fs::WriteFileBytes(path, buffer.data(), buffer.size())) {
        AETHER_LOG_ERROR("Scene", "Failed to write scene file: %s", path.c_str());
        return false;
    }
    AETHER_LOG_INFO("Scene", "Saved %u entities to %s", entity_count, path.c_str());
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
    if (!reader.ReadU32(version) || version != kVersion) {
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
        u64 raw_mask = 0;
        if (!reader.ReadU64(raw_mask)) {
            break;
        }
        ComponentMask file_mask(raw_mask);

        std::vector<ComponentId> ids;
        std::vector<std::vector<u8>> datas;
        ComponentMask resolved_mask;

        for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
            if (!file_mask.test(id)) {
                continue;
            }

            u32 name_len = 0;
            std::string name;
            u32 data_len = 0;
            std::vector<u8> data;
            if (!reader.ReadU32(name_len) || !reader.ReadString(name, name_len) || !reader.ReadU32(data_len)) {
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
            resolved_mask.set(resolved_id);
            ids.push_back(resolved_id);
            datas.push_back(std::move(data));
        }

        Entity entity = world.CreateEntityRaw(resolved_mask);
        for (usize i = 0; i < ids.size(); ++i) {
            const ComponentInfo& info = GetComponentInfo(ids[i]);
            void* ptr = world.GetComponentRaw(entity, ids[i]);
            if (ptr && info.deserialize) {
                info.deserialize(ptr, datas[i].data(), datas[i].size());
            }
        }
        ++loaded;
    }

    AETHER_LOG_INFO("Scene", "Loaded %u entities from %s", loaded, path.c_str());
    return true;
}

} // namespace aether
