# Dedicated servers

`aether_server` hosts a cooked game for networked clients with no window, GPU
or audio device. It mounts the game's `.apak` archives the way `aether_player`
does, loads the startup scene (or the configured one) into a headless player,
and runs the session over UDP: the session host (password, build id, player
limit), replication of the scene to every joined player, and server RPCs. Each
joined player gets a pawn (archetype `aether.player`, owned by that player)
that is removed when they leave.

`aether_netclient` is a headless client for checking a server: it joins,
receives replicated state, calls a server RPC and prints a JSON summary.

## Building and packaging

Both are built with the engine (`tools/server/`). To make a server package from a cook:

    aether_cook Game.aproject --out cooked --config shipping
    tools/package_server.sh --build build --paks cooked --out dist

`dist/` then holds `aether-server-linux-x64.tar.gz`, `SHA256SUMS.txt` (the
archive's SHA-256) and `SERVER_PACKAGE.json` (the size and SHA-256 of the
archive and of every file in it). `--config <file>` packages your own
`server.json` instead of `tools/server/server.json`; `--platform` names the archive.
The archive holds one folder:

    aether-server-linux-x64/
      bin/aether_server
      bin/aether_netclient
      server.json
      Paks/Game.apak
      logs/
      README.txt

The binaries need only glibc and libstdc++ (Ubuntu 24.04 or newer); no Vulkan
loader, X11 or audio libraries. Unpack it anywhere and run `bin/aether_server`:
it reads `server.json` beside itself or one folder up, and relative paths in
that file (`paks`, `log_file`) are relative to the file.

## Configuration

`server.json` is a JSON object (comments allowed). Every key is optional;
unknown keys and wrong types stop the server with exit code 2.

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | `Aether dedicated server` | Session name (64 characters at most). |
| `port` | `7777` | UDP port to listen on (0 picks a free one). |
| `max_players` | `8` | 1 to 1024. Clients beyond this are refused as `Full`. |
| `password` | empty | Needed to join when set. Prefer `AETHER_SERVER_PASSWORD`. |
| `build_id` | `1` | Clients must join with the same build id (`BuildMismatch` otherwise). |
| `scene` | empty | Cooked scene path; empty uses the manifest's startup scene. |
| `paks` | `Paks/` beside the executable | Archives or folders of archives, later ones winning. |
| `tick_rate` | `30` | Fixed ticks per second (1 to 240): one game tick and one network tick each. |
| `log_file` | empty | Append the log here, as well as to stdout. |
| `log_level` | `info` | `trace`, `info`, `warn` or `error`. |
| `replicate_scene` | `true` | Give every scene entity with a Transform a network identity (archetype `scene`). |
| `max_runtime` | `0` | Stop after this many seconds (0: run until signalled). |
| `quit_when_empty` | `false` | Stop once players have joined and all of them have left. |
| `status_interval` | `10` | Seconds between status lines in the log (0: none). |

## Command line

    aether_server [--config <server.json>] [--pak <file|dir>]... [--scene <path>]
                  [--port <n>] [--max-players <n>] [--password <text>] [--build-id <n>]
                  [--name <text>] [--tick-rate <hz>] [--log-file <path>] [--log-level <level>]
                  [--max-runtime <seconds>] [--quit-when-empty] [--no-replicate-scene]
                  [--status-interval <seconds>]

The config file is read first, so command-line values override it; `--pak`
given on the command line replaces the file's `paks`. `AETHER_SERVER_PASSWORD`
overrides both for the password (a password on the command line is visible to
other users of the machine). `AETHER_PAK_KEY` opens encrypted archives.

When the port is bound the server prints `AETHER_SERVER_READY port=<n>`, and
when it stops `AETHER_SERVER_SUMMARY {...}` (why it stopped, ticks, joins,
leaves, refusals, peak players, RPCs run). Ctrl+C or SIGTERM stops it cleanly:
connected clients are told goodbye.

| Exit code | Meaning |
| --- | --- |
| 0 | Clean shutdown (signal, `max_runtime`, `quit_when_empty`, or the game asked to exit). |
| 1 | Startup failed: no archives, an unreadable archive or manifest, or the scene would not load. |
| 2 | Bad configuration or arguments. |
| 3 | The UDP port could not be bound (in use). |

## Ports and firewalls

The server uses one UDP port (`port`, default 7777) for everything; open it for
inbound UDP. There is no TCP port. LAN discovery (UDP 7778) is not answered by
the dedicated server yet.

## Checking a server

    aether_netclient --server 203.0.113.5:7777 --build-id 1 --duration 5

Options: `--name`, `--password` (or `AETHER_SERVER_PASSWORD`), `--build-id`,
`--duration` (seconds to stay after joining), `--join-timeout`, `--no-rpc`.
It prints one line, `AETHER_NETCLIENT` followed by JSON with `joined`,
`join_result` (`Ok`, `WrongPassword`, `BuildMismatch`, `Full`, `Timeout`, ...),
`player`, `entities_seen`, `snapshots`, `pawn_seen`, `rpcs_sent`, `rpc_ack`,
`bytes_sent` and `bytes_received`. It exits 0 when it joined, received
entities and (unless `--no-rpc`) its RPC was acknowledged, else 1.

`tools/server_smoke.sh <dist> <work dir> [port]` is the clean-machine check CI
runs: it verifies the archive's checksum, unpacks it, starts the server, runs
two clients at once and a wrong-password client, and checks the summaries.

## Limits

- Security is the session password, the build id and entity ownership. There
  are no accounts, tokens, encryption or packet signing, so do not expose a
  server holding anything valuable to the internet.
- Clients only see the world after joining; a connected peer that has not
  joined (or was refused) gets no entities and owns nothing to call.
- Gameplay authority is the scene's own scripts running on the server; the
  pawn the server spawns carries only `ServerPlayer` (a ping counter). Games
  that need their own player pawn or spawn rules embed `aether::server::DedicatedServer`
  (`tools/server/include/aether/server/dedicated_server.h`) in their own host.
- Linux x86-64 is the packaged and tested platform. The code is portable, but
  there is no Windows or macOS server package yet.
