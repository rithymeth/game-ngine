#include "aether/kit/kit.h"

#include <algorithm>
#include <map>
#include <set>

namespace aether::kit {

bool KitRegistry::Add(std::unique_ptr<IKit> kit) {
    if (!kit) {
        errors_.push_back("null kit");
        return false;
    }
    const std::string name = kit->Name();
    if (Has(name)) {
        errors_.push_back("duplicate kit \"" + name + "\"");
        return false;
    }
    kits_.push_back(std::move(kit));
    order_.clear();
    return true;
}

bool KitRegistry::Has(const std::string& name) const { return Find(name) != nullptr; }

IKit* KitRegistry::Find(const std::string& name) const {
    for (const auto& kit : kits_) {
        if (name == kit->Name()) return kit.get();
    }
    return nullptr;
}

bool KitRegistry::Resolve() {
    order_.clear();

    // Kahn's algorithm over a sorted set of ready kits, so ties come out alphabetical.
    std::map<std::string, std::set<std::string>> waiting_on; // kit -> dependencies not yet placed
    std::map<std::string, std::vector<std::string>> dependents;
    bool ok = true;
    for (const auto& kit : kits_) {
        const std::string name = kit->Name();
        waiting_on[name];
        for (const std::string& dep : kit->Deps()) {
            if (!Has(dep)) {
                errors_.push_back("kit \"" + name + "\" needs missing kit \"" + dep + "\"");
                ok = false;
                continue;
            }
            waiting_on[name].insert(dep);
            dependents[dep].push_back(name);
        }
    }
    if (!ok) return false;

    std::set<std::string> ready;
    for (const auto& [name, deps] : waiting_on) {
        if (deps.empty()) ready.insert(name);
    }
    while (!ready.empty()) {
        const std::string name = *ready.begin();
        ready.erase(ready.begin());
        order_.push_back(Find(name));
        for (const std::string& dependent : dependents[name]) {
            auto& deps = waiting_on[dependent];
            deps.erase(name);
            if (deps.empty()) ready.insert(dependent);
        }
    }

    if (order_.size() != kits_.size()) {
        std::string stuck;
        for (const auto& [name, deps] : waiting_on) {
            if (!deps.empty()) stuck += (stuck.empty() ? "" : ", ") + name;
        }
        errors_.push_back("kit dependency cycle among: " + stuck);
        order_.clear();
        return false;
    }
    return true;
}

void KitRegistry::RegisterComponents() {
    for (IKit* kit : order_) kit->RegisterComponents();
}

void KitRegistry::LoadAssets(const KitAssetContext& context) {
    for (IKit* kit : order_) kit->LoadAssets(context);
}

void KitRegistry::InstallBlueprintNodes() {
    for (IKit* kit : order_) kit->InstallBlueprintNodes();
}

void KitRegistry::InstallScriptApi(void* script_host) {
    for (IKit* kit : order_) kit->InstallScriptApi(script_host);
}

std::vector<KitStage> KitRegistry::Stages() const {
    std::vector<KitStage> out;
    std::map<std::string, std::string> last_stage; // kit -> its last stage name
    for (IKit* kit : order_) {
        std::vector<KitStage> stages = kit->Stages();
        for (KitStage& stage : stages) {
            for (const std::string& dep : kit->Deps()) {
                auto it = last_stage.find(dep);
                if (it != last_stage.end() &&
                    std::find(stage.after.begin(), stage.after.end(), it->second) == stage.after.end()) {
                    stage.after.push_back(it->second);
                }
            }
        }
        if (!stages.empty()) last_stage[kit->Name()] = stages.back().name;
        out.insert(out.end(), stages.begin(), stages.end());
    }
    return out;
}

} // namespace aether::kit
