#include "aether/gameplay/ability_prediction.h"

#include <algorithm>

namespace aether::gas {

AbilityPredictor::AbilityPredictor(AbilitySystem& abilities) : abilities_(abilities) {}

AbilityPredictor::Prediction AbilityPredictor::Predict(Entity owner, const std::string& ability) {
    Prediction prediction;
    prediction.sequence = next_++;
    const AbilitySystem::ActivateResult result = abilities_.TryActivate(owner, ability);
    prediction.handle = result.handle;
    prediction.reason = result.reason;
    if (result.handle == 0) {
        // Refused locally (unknown, not granted, on cooldown, cannot afford): there is nothing for
        // the server to confirm, so this is a failed press rather than a pending one.
        ++stats_.local_failures;
        return prediction;
    }
    ++stats_.predicted;
    pending_.push_back({prediction.sequence, result.handle});
    return prediction;
}

bool AbilityPredictor::Confirm(Sequence sequence) {
    const auto at = std::find_if(pending_.begin(), pending_.end(), [sequence](const Entry& e) { return e.sequence == sequence; });
    if (at == pending_.end()) {
        // Unknown or already answered. A reply that arrives twice must not pay the cost twice.
        ++stats_.stale_replies;
        return false;
    }
    const AbilityHandle handle = at->handle;
    pending_.erase(at);
    ++stats_.confirmed;
    if (!abilities_.Commit(handle)) {
        // The server said yes but the owner can no longer pay. The ability has already run, so it
        // is left to End/Cancel as usual; count it so the two sides can be compared.
        ++stats_.commit_failed;
        return false;
    }
    return true;
}

bool AbilityPredictor::Reject(Sequence sequence) {
    const auto at = std::find_if(pending_.begin(), pending_.end(), [sequence](const Entry& e) { return e.sequence == sequence; });
    if (at == pending_.end()) {
        ++stats_.stale_replies;
        return false;
    }
    const AbilityHandle handle = at->handle;
    pending_.erase(at);
    ++stats_.rejected;
    abilities_.Cancel(handle); // releases exactly what activation added
    return true;
}

usize AbilityPredictor::DropPending() {
    const usize dropped = pending_.size();
    pending_.clear();
    return dropped;
}

} // namespace aether::gas
