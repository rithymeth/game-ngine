#pragma once

// What an agent may do through the MCP server (Phase 44 steps 3 and 4, docs/design/UPGRADE_PLAN.md): a
// per-project permission file and the three answers it gives for a tool call.
//
//   Allow    run it.
//   Confirm  do not run it yet: answer with a single-use token, and run it only when the same call comes back
//            carrying that token (the client asks the person, then repeats the call with "confirm_token").
//   Deny     refuse it.
//
// The file is JSON, `<project>/.aether/agent_permissions.json`:
//
//   { "$type": "AgentPermissions", "$version": 1, "default": "allow",
//     "tools": { "destroy_entity": "confirm", "save_scene": "deny" } }
//
// A tool named in "tools" gets that answer; any other tool gets "default" when the file has one, else the built-in
// rule: destructive tools (see Tool::destructive) need confirmation and the rest are allowed.

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace aether::mcp {

enum class Permission { Allow, Confirm, Deny };

const char* PermissionName(Permission permission);

// A permission file larger than this is refused before it is parsed.
inline constexpr std::size_t kMaxPermissionFileBytes = 1024 * 1024;

class PermissionPolicy {
public:
    // The built-in rule only: no file.
    PermissionPolicy() = default;

    // Reads a policy from file bytes. False, with a reason, for a file that is too large, not JSON, not an
    // AgentPermissions object, or that names an answer other than allow, confirm or deny. `out` is untouched then.
    [[nodiscard]] static bool FromBytes(std::span<const unsigned char> bytes, PermissionPolicy& out, std::string* error = nullptr);

    Permission For(const std::string& tool, bool destructive) const;

    std::size_t RuleCount() const { return tools_.size(); }

private:
    std::optional<Permission> fallback_;
    std::map<std::string, Permission> tools_;
};

} // namespace aether::mcp
