#pragma once

// @stability: experimental
//
// Typed, frame-local communication between optional gameplay kits (Phase 37.4).
// Producers publish events and consumers drain their own event type. The bus is
// intentionally single-threaded; the player delivers kit events on its update
// thread in scheduler order.

#include <any>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aether::kit {

class KitEventBus {
public:
    template <typename Event>
    void Publish(Event event) {
        const std::type_index type(typeid(Event));
        auto [it, inserted] = pending_.try_emplace(type, std::vector<Event>{});
        (void)inserted;
        std::any_cast<std::vector<Event>&>(it->second).push_back(std::move(event));
    }

    // Returns events in publication order and removes them from the bus.
    template <typename Event>
    std::vector<Event> Drain() {
        const auto it = pending_.find(std::type_index(typeid(Event)));
        if (it == pending_.end()) return {};
        std::vector<Event> out = std::move(std::any_cast<std::vector<Event>&>(it->second));
        pending_.erase(it);
        return out;
    }

    template <typename Event>
    bool HasPending() const {
        return pending_.contains(std::type_index(typeid(Event)));
    }

    void Clear() { pending_.clear(); }

private:
    std::unordered_map<std::type_index, std::any> pending_;
};

} // namespace aether::kit
