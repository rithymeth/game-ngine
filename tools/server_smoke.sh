#!/usr/bin/env bash
# M8.9: the packaged dedicated server on a clean machine. Checks the archive
# against SHA256SUMS.txt, extracts it into an empty folder, starts
# bin/aether_server on localhost (--max-runtime, --quit-when-empty), runs two
# bin/aether_netclient processes against it over real UDP at once, and
# asserts both joined, received replicated state and got their RPC acked,
# and that the server shut down cleanly having seen both. Join tokens are
# required (AETHER_SERVER_TOKEN_SECRET); a wrong password and a forged token
# are refused. Before anyone joins, aether_netclient --discover must find the
# server over LAN discovery (broadcast + 127.0.0.1) with the right advert, and
# one client joins with --discover --join-first. The server writes a network
# profile capture (--profile) whose summary is checked and printed. The server
# log and logs/profile.jsonl are left in <work>/aether-server-*/logs/ for CI to upload.
# Portable: Linux, macOS (bash 3.2) and Windows Git Bash (.tar.gz or .zip; the
# checksum and extraction are done by Python).
#
#   tools/server_smoke.sh <dir with aether-server-*.{tar.gz,zip} and SHA256SUMS.txt> <work dir> [port]
set -euo pipefail
dist="$1" work="$2" port="${3:-7777}"
lan_port=$((port + 1))
# python3 first; on Windows "python3" can be the Store stub, so check that it runs.
py=""
for c in python3 python; do if "$c" -c "" > /dev/null 2>&1; then py="$c"; break; fi; done
[ -n "$py" ] || { echo "server_smoke: needs python3" >&2; exit 1; }
build_id=89 password="m89-smoke"
# Join tokens are on (required) for this run: alpha and bravo get tokens from the packaged server's
# --issue-token, a client with a forged token must be refused.
export AETHER_SERVER_TOKEN_SECRET="m89-smoke-token-issuer-secret-0123456789"

archive=""
for f in "$dist"/aether-server-*.tar.gz "$dist"/aether-server-*.zip; do [ -f "$f" ] && { archive="$(basename "$f")"; break; }; done
[ -n "$archive" ] || { echo "server_smoke: no aether-server-* archive in $dist" >&2; exit 1; }
rm -rf "$work" && mkdir -p "$work"
"$py" - "$dist" "$archive" "$work" <<'PY'
import hashlib, os, sys, tarfile, zipfile
dist, archive, work = sys.argv[1:]
path = os.path.join(dist, archive)
want = {l.split()[1].lstrip("*"): l.split()[0].lower() for l in open(os.path.join(dist, "SHA256SUMS.txt"), encoding="utf-8-sig") if l.strip()}
have = hashlib.sha256(open(path, "rb").read()).hexdigest()
assert want.get(archive) == have, f"{archive}: SHA256SUMS says {want.get(archive)}, file is {have}"
print(f"{archive}: OK ({have})")
if archive.endswith(".zip"):
    with zipfile.ZipFile(path) as z:
        for i in z.infolist():
            z.extract(i, work)
            mode = (i.external_attr >> 16) & 0o777
            if mode: os.chmod(os.path.join(work, i.filename), mode)
else:
    with tarfile.open(path) as t:
        t.extractall(work, filter="data") if hasattr(tarfile, "data_filter") else t.extractall(work)
PY
root="$work/${archive%.tar.gz}"
root="${root%.zip}"
cd "$root"
mkdir -p logs

fail() { echo "server_smoke: FAIL: $*" >&2; echo "--- server stdout"; cat server.out 2>/dev/null || true; echo "--- logs/server.log"; cat logs/server.log 2>/dev/null || true; exit 1; }

# Windows consoles end lines with CRLF: drop the CR from captured tokens.
token_alpha="$(./bin/aether_server --build-id "$build_id" --issue-token acct-alpha --token-ttl 300 | tr -d '\r')" || fail "--issue-token"
token_bravo="$(./bin/aether_server --build-id "$build_id" --issue-token acct-bravo --token-ttl 300 | tr -d '\r')" || fail "--issue-token"
forged="$(AETHER_SERVER_TOKEN_SECRET=not-the-real-issuer-secret ./bin/aether_server --build-id "$build_id" --issue-token acct-eve | tr -d '\r')"
case "$token_alpha" in aet1.*) ;; *) fail "unexpected token '$token_alpha'";; esac

AETHER_SERVER_PASSWORD="$password" ./bin/aether_server --port "$port" --build-id "$build_id" --max-runtime 60 \
    --quit-when-empty --status-interval 1 --lan-port "$lan_port" --profile logs/profile.jsonl --profile-interval 0.5 \
    > server.out 2>&1 &
server=$!
for _ in $(seq 1 100); do
    grep -q "^AETHER_SERVER_READY" server.out && break
    kill -0 "$server" 2>/dev/null || fail "the server exited before it was ready"
    sleep 0.1
done
grep -q "^AETHER_SERVER_READY port=$port lan_port=$lan_port" server.out || fail "the server never said it was ready (and answers LAN discovery)"

# LAN discovery before anyone joins: the server is listed with its advert.
./bin/aether_netclient --discover --lan-port "$lan_port" --build-id "$build_id" --timeout 1 > discover.out 2>&1 || fail "--discover found nothing: $(cat discover.out)"
cat discover.out
"$py" - discover.out "$port" "$build_id" <<'PY' || fail "discovery result"
import json, sys
line = next(l for l in open(sys.argv[1]) if l.startswith("AETHER_DISCOVER "))
r = json.loads(line.split(" ", 1)[1])
mine = [s for s in r["servers"] if s["port"] == int(sys.argv[2])]
assert mine, r
s = mine[0]
assert s["compatible"] and s["build"] == int(sys.argv[3]) and s["players"] == 0 and s["max_players"] == 8, s
assert s["requires_token"] and s["password"] and s["mode"] == "dedicated", s
print(f"discovered '{s['name']}' at {s['address']}: {s['players']}/{s['max_players']} players, build {s['build']}, "
      f"password {s['password']}, requires_token {s['requires_token']}, ping {s['ping_ms']:.2f} ms ({r['count']} answer(s))")
PY

run_client() {
    AETHER_SERVER_PASSWORD="$password" ./bin/aether_netclient --server "127.0.0.1:$port" --build-id "$build_id" \
        --name "$1" --token "$2" --duration 3 > "client-$1.out" 2>&1
}
run_client alpha "$token_alpha" & c1=$!
# bravo finds the server by LAN discovery and joins the first compatible one.
AETHER_SERVER_PASSWORD="$password" ./bin/aether_netclient --discover --join-first --lan-port "$lan_port" --timeout 1 \
    --build-id "$build_id" --name bravo --token "$token_bravo" --duration 3 > client-bravo.out 2>&1 & c2=$!
# While they play (--quit-when-empty ends the server once they leave), a wrong password must be refused.
s3=0
./bin/aether_netclient --server "127.0.0.1:$port" --build-id "$build_id" --password nope --token "$token_alpha" \
    --duration 1 > client-bad.out 2>&1 || s3=$?
# ...and so must a forged token, even with the right password.
s4=0
AETHER_SERVER_PASSWORD="$password" ./bin/aether_netclient --server "127.0.0.1:$port" --build-id "$build_id" --name eve \
    --token "$forged" --duration 1 > client-forged.out 2>&1 || s4=$?
s1=0 s2=0
wait "$c1" || s1=$?
wait "$c2" || s2=$?
cat client-alpha.out client-bravo.out client-bad.out client-forged.out
[ "$s1" = 0 ] && [ "$s2" = 0 ] || fail "client exit codes $s1 $s2"
[ "$s3" = 1 ] || fail "a wrong password was let in (exit $s3)"
grep -q '"join_result":"WrongPassword"' client-bad.out || fail "the wrong password wasn't refused as WrongPassword"
grep -q '"entities_seen":0,' client-bad.out || fail "a refused client was sent replicated state"
[ "$s4" = 1 ] || fail "a forged token was let in (exit $s4)"
grep -q '"join_result":"BadToken"' client-forged.out || fail "the forged token wasn't refused as BadToken"
grep -q '"entities_seen":0,' client-forged.out || fail "a forged-token client was sent replicated state"
grep -q "^AETHER_DISCOVER " client-bravo.out || fail "bravo didn't discover"
"$py" - client-alpha.out client-bravo.out <<'PY' || fail "client summaries"
import json, sys
for path in sys.argv[1:]:
    line = next(l for l in open(path) if l.startswith("AETHER_NETCLIENT "))
    r = json.loads(line.split(" ", 1)[1])
    assert r["joined"] and r["pawn_seen"] and r["rpc_ack"], r
    assert r["entities_seen"] >= 3 and r["snapshots"] > 0 and r["bytes_received"] > 0, r
    print(f"{path}: joined as {r['player']}, {r['entities_seen']} entities, {r['snapshots']} snapshots, "
          f"{r['bytes_received']} B received, rpc ack in {r['rpc_ack_seconds']:.3f} s")
PY

status=0
wait "$server" || status=$?
cat server.out
[ "$status" = 0 ] || fail "server exit code $status"
grep -q '^AETHER_SERVER_SUMMARY {"reason":"empty".*"joins":2,.*"refused":2,.*"pings":2' server.out || fail "server summary"
grep -q '"lan_answered":[1-9]' server.out || fail "the server answered no LAN discovery"
test -s logs/server.log || fail "no server log"
"$py" - logs/profile.jsonl <<'PY' || fail "profile capture"
import json, sys
records = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
summary = records[-1]["summary"]
samples = records[:-1]
assert len(samples) >= 4 and summary["records"] == len(samples), (len(samples), summary)
assert summary["peak_peers"] >= 2 and summary["snapshots"] > 0 and summary["bytes_sent"] > 0, summary
assert any(len(r["peers"]) >= 2 and all(len(p["channels"]) == 3 for p in r["peers"]) for r in samples)
assert summary["lan_answered"] >= 1, summary
print(f"profile: {len(samples)} records, peak {summary['peak_peers']} peers, rtt mean {summary['rtt_ms_mean']:.2f} ms "
      f"max {summary['rtt_ms_max']:.2f} ms, loss max {summary['loss_max']:.3f}, {summary['snapshots']} snapshots "
      f"avg {summary['avg_snapshot_bytes']:.1f} B, sent {summary['bytes_sent']} B, received {summary['bytes_received']} B, "
      f"rate_limited {summary['rate_limited']}, bad_macs {summary['bad_macs']}, lan answered {summary['lan_answered']}")
PY
echo "server_smoke: OK"
