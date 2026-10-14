#pragma once

#include "aether/ecs/entity.h"

#include <string>
#include <vector>

namespace aether::editor::qt {

struct BehaviorTreeDebugNode {
    std::string path;
    std::string label;
    std::string status;
    bool active = false;
};

struct BehaviorTreeDebugKey {
    std::string name;
    std::string type;
    std::string value;
};

struct BehaviorTreeDebugSnapshot {
    Entity entity;
    std::string tree;
    bool running = false;
    std::string active_node;
    std::vector<BehaviorTreeDebugNode> nodes;
    std::vector<BehaviorTreeDebugKey> blackboard;
};

} // namespace aether::editor::qt
