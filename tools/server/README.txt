Aether dedicated server
=======================

  bin/aether_server        the server (headless: no window, GPU or audio device needed)
  bin/aether_netclient     a headless test client
  server.json              the config (every key can be overridden on the command line)
  Paks/                    the cooked game

Run:      bin/aether_server                       (UDP 7777, reads ./server.json and ./Paks)
          bin/aether_server --port 27015 --max-players 16 --build-id 89
          AETHER_SERVER_PASSWORD=secret bin/aether_server
Tokens:   AETHER_SERVER_TOKEN_SECRET=<32+ random chars> bin/aether_server          (joins need a signed token)
          AETHER_SERVER_TOKEN_SECRET=... bin/aether_server --issue-token player-1 --token-ttl 600
          bin/aether_netclient --server 127.0.0.1:7777 --token aet1....
Stop:     Ctrl+C or SIGTERM (clean shutdown, exit code 0)
Check:    bin/aether_netclient --server 127.0.0.1:7777 --build-id 1 --duration 3
          (prints one AETHER_NETCLIENT {...} JSON line; exit 0 when it joined, saw state and got its RPC acked)

Find:     bin/aether_netclient --discover [--join-first ...]   (LAN discovery, UDP 7778; --no-lan-advertise turns it off)
Profile:  bin/aether_server --profile logs/profile.jsonl        (per-peer RTT/loss/bytes per channel, snapshot sizes)

Open UDP port 7777 (or your "port") in the firewall, and 7778 ("lan_port") for LAN discovery. Logs go to logs/server.log.
Exit codes: 0 clean shutdown, 1 startup failure, 2 bad config or arguments, 3 port in use.
Requirements: Linux x86-64 with glibc and libstdc++ (Ubuntu 24.04 or newer), Windows x64, or macOS arm64 (per package).
On Windows the binaries are bin\aether_server.exe and bin\aether_netclient.exe.
Full reference: docs/manual/15-dedicated-server.md in the engine.
