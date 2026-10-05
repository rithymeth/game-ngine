#include "aether/loc/localization.h"

#include <algorithm>
#include <atomic>

namespace aether::loc {

namespace {
std::atomic<Localization*> g_active{nullptr};
}

Localization::~Localization() {
    Localization* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

Localization* Localization::Active() { return g_active.load(); }
void Localization::MakeActive() { g_active.store(this); }
void Localization::ClearActive() { g_active.store(nullptr); }

bool Localization::SetLanguage(std::string language) {
    const std::string before = localizer_.Language();
    localizer_.SetLanguage(std::move(language));
    const std::string now = localizer_.Language();
    if (now == before) return false;
    const auto listeners = listeners_; // a listener may add or remove listeners
    for (const auto& [id, fn] : listeners) fn(now, before);
    return true;
}

u64 Localization::AddListener(Listener fn) {
    listeners_.emplace_back(++next_id_, std::move(fn));
    return next_id_;
}

void Localization::RemoveListener(u64 id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](const auto& l) { return l.first == id; }), listeners_.end());
}

} // namespace aether::loc
