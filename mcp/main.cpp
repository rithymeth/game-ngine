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
#include "audio_tools.h"
#include "build_tools.h"
#include "editor_tools.h"
#include "game_tools.h"
#include "kit_tools.h"
#include "nav_tools.h"
#include "terrain_tools.h"
#include "vfx_tools.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <iostream>

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
        } else {
            std::fprintf(stderr, "usage: aether_mcp_server [--scene scene.json] [--build-dir <cmake build dir>]\n");
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
    mcp::RegisterTerrainTools(server);
    mcp::RegisterNavTools(server, session);
    std::error_code ec;
    mcp::RegisterBuildTools(server, mcp::FindBuildContext(build_dir, std::filesystem::absolute(argv[0], ec).parent_path()));
    server.RunStdio(std::cin, std::cout);
    return 0;
}
