#include "permissions.h"

#include "aether/core/json_util.h"

#include <nlohmann/json.hpp>

namespace aether::mcp {

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

bool ParsePermission(const nlohmann::json& value, Permission& out) {
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    if (text == "allow") out = Permission::Allow;
    else if (text == "confirm") out = Permission::Confirm;
    else if (text == "deny") out = Permission::Deny;
    else return false;
    return true;
}

} // namespace

const char* PermissionName(Permission permission) {
    switch (permission) {
    case Permission::Allow: return "allow";
    case Permission::Confirm: return "confirm";
    case Permission::Deny: return "deny";
    }
    return "deny";
}

bool PermissionPolicy::FromBytes(std::span<const unsigned char> bytes, PermissionPolicy& out, std::string* error) {
    if (bytes.size() > kMaxPermissionFileBytes) return Fail(error, "the permission file is larger than 1 MiB");
    return GuardJsonLoader(error, "permission file", [&] {
        const nlohmann::json json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded() || !json.is_object() || JsonString(json, "$type") != "AgentPermissions") {
            return Fail(error, "the permission file is not an AgentPermissions JSON object");
        }
        PermissionPolicy policy;
        if (const auto it = json.find("default"); it != json.end()) {
            Permission value;
            if (!ParsePermission(*it, value)) return Fail(error, "\"default\" must be \"allow\", \"confirm\" or \"deny\"");
            policy.fallback_ = value;
        }
        if (const auto it = json.find("tools"); it != json.end()) {
            if (!it->is_object()) return Fail(error, "\"tools\" must be an object of tool name to answer");
            for (auto tool = it->begin(); tool != it->end(); ++tool) {
                Permission value;
                if (!ParsePermission(tool.value(), value)) {
                    return Fail(error, "the answer for \"" + tool.key() + "\" must be \"allow\", \"confirm\" or \"deny\"");
                }
                policy.tools_[tool.key()] = value;
            }
        }
        out = std::move(policy);
        return true;
    });
}

Permission PermissionPolicy::For(const std::string& tool, bool destructive) const {
    if (const auto it = tools_.find(tool); it != tools_.end()) return it->second;
    if (fallback_) return *fallback_;
    return destructive ? Permission::Confirm : Permission::Allow;
}

} // namespace aether::mcp
