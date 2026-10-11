// aether_mcp_server: a Model Context Protocol server for the Aether editor's
// scene, over stdio. Point an MCP client at it, e.g. in Claude Code:
//
//   claude mcp add aether -- aether_mcp_server [--scene scene.json]
//
// stdout carries the protocol only; engine logging below Error is switched
// off (errors go to stderr).

#include "aether/core/log.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"
#include "editor_tools.h"
#include "permissions.h"

#include "aether/platform/filesystem.h"

#include <cstdio>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <memory>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    using namespace aether;

    Logger::Instance().SetMinLevel(LogLevel::Error);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); // no CRLF translation on the protocol streams
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    mcp::RegisterBuiltinComponents();
    mcp::EditorSession session;
    std::error_code cwd_error;
    session.scene_root = std::filesystem::current_path(cwd_error); // scene tools stay in the working folder unless --scene-root says otherwise

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            const char* path = argv[++i];
            if (!LoadSceneJson(session.world, path)) {
                std::fprintf(stderr, "aether_mcp_server: could not load scene %s\n", path);
                return 1;
            }
            editor::EnsureAllGuids(session.world, session.guids);
        } else if (std::strcmp(argv[i], "--scene-root") == 0 && i + 1 < argc) {
            session.scene_root = argv[++i];
        } else {
            std::fprintf(stderr, "usage: aether_mcp_server [--scene scene.json] [--scene-root folder]\n");
            return 1;
        }
    }

    mcp::McpServer server("aether-editor", "0.1.0");
    mcp::RegisterEditorTools(server, session);

    // Permissions and the session log live in <scene folder>/.aether/. A missing permission file means the
    // built-in rule (destructive tools ask first); an unreadable one is reported and the same rule applies.
    const std::filesystem::path state_dir = session.scene_root / ".aether";
    {
        mcp::PermissionPolicy policy;
        std::vector<u8> bytes;
        const std::filesystem::path file = state_dir / "agent_permissions.json";
        if (fs::ReadFileBytes(file.string(), bytes)) {
            std::string error;
            if (!mcp::PermissionPolicy::FromBytes(bytes, policy, &error)) {
                std::fprintf(stderr, "aether_mcp_server: %s: %s; using the built-in rule\n", file.string().c_str(), error.c_str());
                policy = mcp::PermissionPolicy();
            }
        }
        server.SetPolicy(std::move(policy));
    }
    {
        // One JSON record per line, capped so a long session cannot fill the disk.
        const std::filesystem::path log_file = state_dir / "agent_session.jsonl";
        auto written = std::make_shared<std::uintmax_t>(0);
        std::error_code ec;
        std::filesystem::create_directories(state_dir, ec);
        if (const auto size = std::filesystem::file_size(log_file, ec); !ec) *written = size;
        constexpr std::uintmax_t kMaxLogBytes = 8u * 1024u * 1024u;
        server.SetAuditSink([log_file, written](const mcp::Json& record) {
            if (*written >= kMaxLogBytes) return;
            std::ofstream out(log_file, std::ios::app);
            if (!out) return;
            const std::string line = record.dump();
            out << line << '\n';
            *written += line.size() + 1;
        });
    }
    server.RunStdio(std::cin, std::cout);
    return 0;
}
