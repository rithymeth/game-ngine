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
#include "asset_tools.h"
#include "blueprint_tools.h"
#include "audio_tools.h"
#include "build_tools.h"
#include "editor_tools.h"
#include "game_tools.h"
#include "kit_tools.h"
#include "loc_tools.h"
#include "script_tools.h"
#include "sequence_tools.h"
#include "tilemap_tools.h"
#include "nav_tools.h"
#include "terrain_tools.h"
#include "animation_tools.h"
#include "ui_tools.h"
#include "save_tools.h"
#include "streaming_tools.h"
#include "material_tools.h"
#include "prefab_tools.h"
#include "vfx_tools.h"
#include "permissions.h"

#include "aether/platform/filesystem.h"

#include <cstdio>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <string>
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

    std::string build_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--build-dir") == 0 && i + 1 < argc) {
            build_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            const char* path = argv[++i];
            if (!LoadSceneJson(session.world, path)) {
                std::fprintf(stderr, "aether_mcp_server: could not load scene %s\n", path);
                return 1;
            }
            editor::EnsureAllGuids(session.world, session.guids);
        } else if (std::strcmp(argv[i], "--scene-root") == 0 && i + 1 < argc) {
            session.scene_root = argv[++i];
        } else {
            std::fprintf(stderr, "usage: aether_mcp_server [--scene scene.json] [--scene-root folder] [--build-dir <cmake build dir>]\n");
            return 1;
        }
    }

    mcp::McpServer server("aether-editor", "0.1.0");
    mcp::RegisterEditorTools(server, session);
    mcp::RegisterGameTools(server);
    auto project = mcp::MakeAssetHost(); // the open project, shared by the asset and kit tools
    mcp::RegisterAssetTools(server, project);
    mcp::RegisterKitTools(server, project);
    mcp::RegisterAudioTools(server, project);
    mcp::RegisterVfxTools(server, project);
    mcp::RegisterBlueprintTools(server, project);
    mcp::RegisterLocTools(server, project);
    mcp::RegisterSequenceTools(server, project);
    mcp::RegisterScriptTools(server, project);
    mcp::RegisterAnimationTools(server, project);
    mcp::RegisterUiTools(server, project);
    mcp::RegisterSaveTools(server);
    mcp::RegisterStreamingTools(server, session);
    mcp::RegisterMaterialTools(server, project);
    mcp::RegisterPrefabTools(server, project, session);
    mcp::RegisterTilemapTools(server, project);
    mcp::RegisterTerrainTools(server);
    mcp::RegisterNavTools(server, session);
    std::error_code ec;
    mcp::RegisterBuildTools(server, mcp::FindBuildContext(build_dir, std::filesystem::absolute(argv[0], ec).parent_path()));

    // Work undo cannot bring back, from the tool groups above: the built-in permission rule asks the person before these run.
    for (const char* name : {"asset_delete", "save_delete", "prefab_apply"}) server.MarkDestructive(name);

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
