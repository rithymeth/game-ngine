#pragma once

#include "aether/assets/asset_guid.h"

#include <memory>
#include <unordered_map>

namespace aether::assets {

template <typename T>
class AssetStore;

// A live reference to loaded asset data (docs/design/PHASE_SPECS.md §8.3).
// Hot reload replaces the data behind every handle at once and bumps the
// generation, so code that built something from the data (a GPU texture, a
// physics mesh) compares generations and rebuilds when it changed:
//
//   if (handle.Generation() != built_generation) { Rebuild(*handle.Get()); ... }
//
// Handles stay valid after the store drops the asset; they then keep the last
// data they saw.
template <typename T>
class AssetHandle {
public:
    AssetHandle() = default;

    const T* Get() const { return slot_ && slot_->data ? slot_->data.get() : nullptr; }
    const T* operator->() const { return Get(); }
    bool IsLoaded() const { return Get() != nullptr; }
    // 0 until first loaded; +1 on every (re)load.
    u32 Generation() const { return slot_ ? slot_->generation : 0; }
    AssetGuid Guid() const { return slot_ ? slot_->guid : AssetGuid{}; }
    bool IsValid() const { return slot_ != nullptr; }

private:
    friend class AssetStore<T>;
    struct Slot {
        AssetGuid guid;
        std::shared_ptr<const T> data;
        u32 generation = 0;
    };
    explicit AssetHandle(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}

    std::shared_ptr<Slot> slot_;
};

// The loaded data of one kind of asset (textures, meshes, ...), by GUID.
template <typename T>
class AssetStore {
public:
    // The handle for `guid`; the same slot for every call, loaded or not.
    AssetHandle<T> Acquire(const AssetGuid& guid) { return AssetHandle<T>(SlotFor(guid)); }

    // Loads or replaces the data: every handle sees the new data, with the
    // generation bumped.
    void Set(const AssetGuid& guid, T data) {
        auto slot = SlotFor(guid);
        slot->data = std::make_shared<const T>(std::move(data));
        ++slot->generation;
    }

    bool Contains(const AssetGuid& guid) const {
        auto it = slots_.find(guid);
        return it != slots_.end() && it->second->data != nullptr;
    }
    void Remove(const AssetGuid& guid) { slots_.erase(guid); }
    usize Size() const { return slots_.size(); }

private:
    using Slot = typename AssetHandle<T>::Slot;
    std::shared_ptr<Slot> SlotFor(const AssetGuid& guid) {
        std::shared_ptr<Slot>& slot = slots_[guid];
        if (!slot) {
            slot = std::make_shared<Slot>();
            slot->guid = guid;
        }
        return slot;
    }

    std::unordered_map<AssetGuid, std::shared_ptr<Slot>> slots_;
};

} // namespace aether::assets
