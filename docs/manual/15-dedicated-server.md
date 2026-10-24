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

`dist/` then holds the archive, `SHA256SUMS.txt` (its SHA-256) and
`SERVER_PACKAGE.json` (the platform, size and SHA-256 of the archive and of
every file in it). The script runs under bash on Linux, macOS and Windows (Git
Bash) and needs Python 3; the platform is detected, or given with `--platform`:

| Platform | Archive | Built by |
| --- | --- | --- |
| `linux-x64` | `aether-server-linux-x64.tar.gz` | GCC (CI `linux` job, smoke in a bare `ubuntu:24.04` container) |
| `windows-x64` | `aether-server-windows-x64.zip` | MSVC (CI `windows` job; binaries from `build/tools/server/Release/`) |
| `macos-arm64` | `aether-server-macos-arm64.tar.gz` | Apple clang (CI `macos` job) |

Archives are reproducible (sorted names, fixed owner, mtime from the last
commit or `SOURCE_DATE_EPOCH`). `--config <file>` packages your own
`server.json` instead of `tools/server/server.json`; `--no-strip` keeps
symbols in the Linux/macOS binaries. Tagged releases publish all three
archives (and their `SERVER_PACKAGE.json` as `aether-server-<platform>.json`),
listed in the release `SHA256SUMS.txt` and covered by its build-provenance
attestation. The archive holds one folder (`.exe` binaries on Windows):

    aether-server-linux-x64/
      bin/aether_server
      bin/aether_netclient
      server.json
      Paks/Game.apak
      logs/
      README.txt

On Linux the binaries need only glibc and libstdc++ (Ubuntu 24.04 or newer);
no Vulkan loader, X11 or audio libraries. The Windows build needs the MSVC
runtime the engine build links; the macOS build targets Apple Silicon. Unpack it anywhere and run `bin/aether_server`:
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
| `token_secret` | empty | Join-token issuer secret (16+ characters; use 32+ random ones). Empty: no tokens. Prefer `AETHER_SERVER_TOKEN_SECRET`. |
| `require_token` | `true` | With a `token_secret`: refuse joins without a valid token (`false`: check a token only when one is sent). |
| `scene` | empty | Cooked scene path; empty uses the manifest's startup scene. |
| `paks` | `Paks/` beside the executable | Archives or folders of archives, later ones winning. |
| `tick_rate` | `30` | Fixed ticks per second (1 to 240): one game tick and one network tick each. |
| `log_file` | empty | Append the log here, as well as to stdout. |
| `log_level` | `info` | `trace`, `info`, `warn` or `error`. |
| `replicate_scene` | `true` | Give every scene entity with a Transform a network identity (archetype `scene`). |
| `max_runtime` | `0` | Stop after this many seconds (0: run until signalled). |
| `quit_when_empty` | `false` | Stop once players have joined and all of them have left. |
| `status_interval` | `10` | Seconds between status lines in the log (0: none). |
| `lan_advertise` | `true` | Answer LAN discovery queries on `lan_port` (see LAN discovery). |
| `lan_port` | `7778` | UDP port for LAN discovery (0: a free one). If it is taken (another server on this host) the server logs a warning and runs without advertising. |
| `profile` | empty | Write a network profile capture (JSON lines) to this file. |
| `profile_interval` | `1` | Seconds between capture records (> 0). |

## Command line

    aether_server [--config <server.json>] [--pak <file|dir>]... [--scene <path>]
                  [--port <n>] [--max-players <n>] [--password <text>] [--build-id <n>]
                  [--name <text>] [--tick-rate <hz>] [--log-file <path>] [--log-level <level>]
                  [--max-runtime <seconds>] [--quit-when-empty] [--no-replicate-scene]
                  [--status-interval <seconds>] [--token-secret <text>] [--require-token | --optional-token]
                  [--lan-port <n>] [--no-lan-advertise] [--profile <file.jsonl>] [--profile-interval <seconds>]
    aether_server [--config <server.json>] [--token-secret <text>] [--build-id <n>]
                  --issue-token <player id> [--token-ttl <seconds>]

The config file is read first, so command-line values override it; `--pak`
given on the command line replaces the file's `paks`. `AETHER_SERVER_PASSWORD`
overrides both for the password (a password on the command line is visible to
other users of the machine); `AETHER_SERVER_TOKEN_SECRET` does the same for
`token_secret`. `--issue-token` prints a join token for that player id, valid
for `--token-ttl` seconds (default 3600) on `build_id`, and exits; it is meant
for testing and small setups, a real backend issues tokens itself (see
Security). `AETHER_PAK_KEY` opens encrypted archives.

When the port is bound the server prints `AETHER_SERVER_READY port=<n> lan_port=<n>`
(`lan_port=0`: not advertising), and when it stops `AETHER_SERVER_SUMMARY {...}`
(why it stopped, ticks, joins, leaves, refusals, peak players, RPCs run, the
security drop counters `rate_limited`, `bad_cookies`, `replayed_handshakes`,
`bad_macs`, and `lan_answered` / `lan_rate_limited`). With `--profile` it also
prints `AETHER_SERVER_PROFILE {...}` (the capture summary). Ctrl+C or SIGTERM
stops it cleanly, and on Windows so do Ctrl+Break and closing the console
(`SetConsoleCtrlHandler`): connected clients are told goodbye.

| Exit code | Meaning |
| --- | --- |
| 0 | Clean shutdown (signal, `max_runtime`, `quit_when_empty`, or the game asked to exit). |
| 1 | Startup failed: no archives, an unreadable archive or manifest, or the scene would not load. |
| 2 | Bad configuration or arguments. |
| 3 | The UDP port could not be bound (in use). |

## Ports and firewalls

The server uses one UDP port (`port`, default 7777) for the game; open it for
inbound UDP. LAN discovery uses a second UDP port (`lan_port`, default 7778);
open it only on networks where clients should find the server by broadcast,
or turn it off with `--no-lan-advertise`. There is no TCP port.

## LAN discovery

The server answers discovery queries on `lan_port` with its advert: `name`,
`players` (kept current), `max_players`, `build`, the game `port`, `mode`
(`dedicated`), whether a password is needed (never the password) and a
`requires_token` property. Find servers with:

    aether_netclient --discover [--timeout 1] [--lan-port 7778] [--query <ip:port>]... [--build-id <n>]

It broadcasts to `255.255.255.255:<lan_port>` and also queries `127.0.0.1`
(and every `--query` address: use it for a known host, or where broadcast
does not reach), repeating every 0.25 s for `--timeout` seconds, then prints
`AETHER_DISCOVER {"count":..,"servers":[{"address","name","players","max_players","build","port","compatible","password","requires_token","ping_ms",...}]}`
and exits 0 if any answered. A server reached both by broadcast and by
127.0.0.1 is listed once per address. `--discover --join-first` then joins
the first compatible server (lowest ping) with the usual join options.

Discovery replies are unauthenticated, so they follow the same
anti-amplification rules as the handshake: queries are padded to 1024 bytes
and an answer is never bigger than the query (shorter queries, as sent by
builds before this change, get no answer); each source IP gets 4 answers a
second (burst 8) from a table of at most 1024 addresses. The advert is
information, not authority: joining still needs the password, token and
build id.

## Network profile capture

`--profile logs/profile.jsonl` (or `profile` in `server.json`) writes one JSON
object per `profile_interval`:

- `t`, `players`;
- `peers[]`: `peer`, `address`, `player`, `rtt_ms`, `loss` (smoothed),
  `bytes_sent` / `bytes_received`, `packets_sent` / `packets_received` /
  `packets_lost`, `messages_resent`, and `channels[]` with message bytes sent
  and received per channel (0 reliable session/RPC, 1 unreliable, 2 sequenced
  snapshots; headers and resends included, so they sum to less than the
  packet bytes);
- `interval`: seconds, `snapshots`, `snapshot_bytes`, `avg_snapshot_bytes`
  and bytes sent/received across peers since the previous record;
- `security`: the `HostSecurityStats` counters (`rate_limited`,
  `challenges_sent`, `bad_cookies`, `replayed_handshakes`, `bad_macs`,
  `malformed`);
- `lan`: discovery `answered`, `rate_limited`, `oversized`.

The last line is `{"summary":{...}}`: records, peak peers, mean/max RTT, max
loss, snapshots and their mean size, total bytes, and the security and LAN
counters. Byte deltas count peers present at a record; a peer that connects
and leaves between two records (a refused join, say) is not in them.

## Checking a server

    aether_netclient --server 203.0.113.5:7777 --build-id 1 --duration 5

Options: `--name`, `--password` (or `AETHER_SERVER_PASSWORD`), `--token` (or
`AETHER_JOIN_TOKEN`), `--build-id`,
`--duration` (seconds to stay after joining), `--join-timeout`, `--no-rpc`.
It prints one line, `AETHER_NETCLIENT` followed by JSON with `joined`,
`join_result` (`Ok`, `WrongPassword`, `BuildMismatch`, `Full`, `BadToken`, `TokenExpired`, `Timeout`, ...),
`player`, `entities_seen`, `snapshots`, `pawn_seen`, `rpcs_sent`, `rpc_ack`,
`bytes_sent` and `bytes_received`. It exits 0 when it joined, received
entities and (unless `--no-rpc`) its RPC was acknowledged, else 1.

`tools/server_smoke.sh <dist> <work dir> [port]` is the clean-machine check CI
runs on Linux (bare container), Windows (Git Bash) and macOS: it verifies the
archive's checksum, unpacks it, starts the server with `--profile`, checks
that `--discover` finds it with the right advert, runs two clients at once
with join tokens from `--issue-token` (one joining by `--discover
--join-first`), a wrong-password client and a forged-token client, and checks
the summaries and the profile capture. The `dedicated-server-profile*` CI
artifacts hold the server log and capture.

## Security

What the transport and session do (`aether::net::NetHost`, `SessionHost`):

- **Stateless connection challenge.** A client's `ConnectRequest` is padded to
  69 bytes. The server answers with a 29-byte challenge holding a cookie:
  SipHash-2-4 under a per-process random secret over the client's address,
  its nonce and the time. It keeps no state. Only a `ConnectResponse` from the
  same address echoing a valid cookie younger than 10 s (`HostConfig::cookie_lifetime`)
  creates a peer. A spoofed source address never sees the cookie, so it can't
  complete a handshake or use up peer slots, and no reply before verification
  is bigger than the request (no amplification). Each cookie works once: a
  replayed handshake after the peer is gone is refused (`replayed_handshakes`).
- **Packet authentication.** The Accept carries two per-session, per-direction
  128-bit keys derived from the server secret, both nonces and the client
  address. Every `Data` and `Disconnect` datagram after that carries an 8-byte
  SipHash-2-4 tag. Forged, corrupted or spoofed frames, including a spoofed
  goodbye, are dropped and counted (`bad_macs`) in both directions. Each
  datagram is 8 bytes bigger, which adds about 250-500 B/s per client in the
  recorded scenario and stays inside the existing 16000 B/s budget.
- **Join tokens.** With `token_secret` set, a client must send a token
  (`aether_netclient --token`, `SessionClient::Join(..., token)`). Its text is
  `aet1.<hex payload>.<hex tag>`. The payload holds a version, an expiry (Unix
  seconds), the build id and a player id of up to 64 bytes. The tag is
  SipHash-2-4 under a key derived from the secret. A bad signature, the wrong
  build or a malformed token is refused as `BadToken`, an old one as
  `TokenExpired`. The player id becomes `SessionPlayer::account`. A token is
  checked in addition to `password` (if both are set, both are needed). To
  issue tokens from a backend, call `aether::net::IssueJoinToken` (C++) or port
  the format above. Tokens are bearer credentials: they aren't bound to an
  address and can be reused until they expire, so keep TTLs short.
- **Per-address rate limits.** Token buckets per source `ip:port`:
  4 handshakes/s (burst 8), 1000 datagrams/s (burst 2000) and 512 KiB/s
  (burst 1 MiB), in a table of at most 4096 addresses where the least
  recently seen one is evicted. Anything over budget is dropped before it is
  parsed further and counted (`rate_limited`). The limits are fields of `HostConfig`.

What it does not do:

- **No encryption.** Traffic, including the session keys in the Accept, is
  plaintext. The MAC stops off-path spoofing and injection, the common case
  on the internet. Anyone who can read the traffic (same Wi-Fi, a hostile
  router) can read and forge it. A key exchange such as X25519 plus an AEAD
  is the follow-up.
- Rate limits are per `ip:port`. An attacker with many real source addresses
  (a botnet) is limited by the address table and the stateless handshake,
  not blocked. Put a provider's DDoS protection in front of public servers.
- Tokens can't be revoked before they expire. The server keeps no account
  database.
- The wire protocol changed with this hardening: older clients can't connect
  to a new server, or the other way round. `build_id` should change with it.

## Limits

- Authority is the session (password and/or join token), the build id and
  entity ownership, over the authenticated (not encrypted) transport above.
- Clients only see the world after joining; a connected peer that has not
  joined (or was refused) gets no entities and owns nothing to call.
- Gameplay authority is the scene's own scripts running on the server; the
  pawn the server spawns carries only `ServerPlayer` (a ping counter). Games
  that need their own player pawn or spawn rules embed `aether::server::DedicatedServer`
  (`tools/server/include/aether/server/dedicated_server.h`) in their own host.
- Packages and smoke tests cover Linux x86-64, Windows x64 and macOS arm64,
  each on one machine over localhost. No cross-machine or NAT run is
  recorded yet, and discovery is LAN broadcast only (no internet lobby).
