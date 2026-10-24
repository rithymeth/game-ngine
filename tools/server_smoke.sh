#!/usr/bin/env bash
# M8.9: the packaged dedicated server on a clean machine. Checks the archive
# against SHA256SUMS.txt, extracts it into an empty folder, starts
# bin/aether_server on localhost (--max-runtime, --quit-when-empty), runs two
# bin/aether_netclient processes against it over real UDP at once, and
# asserts both joined, received replicated state and got their RPC acked,
# and that the server shut down cleanly having seen both. Join tokens are
# required (AETHER_SERVER_TOKEN_SECRET); a wrong password and a forged token
# are refused. The server log is
# left in <work>/aether-server-*/logs/server.log for CI to upload.
#
#   tools/server_smoke.sh <dir with aether-server-*.tar.gz and SHA256SUMS.txt> <work dir> [port]
set -euo pipefail
dist="$1" work="$2" port="${3:-7777}"
build_id=89 password="m89-smoke"
# Join tokens are on (required) for this run: alpha and bravo get tokens from the packaged server's
# --issue-token, a client with a forged token must be refused.
export AETHER_SERVER_TOKEN_SECRET="m89-smoke-token-issuer-secret-0123456789"

archive="$(cd "$dist" && ls aether-server-*.tar.gz | head -1)"
(cd "$dist" && sha256sum -c SHA256SUMS.txt)
rm -rf "$work" && mkdir -p "$work"
tar -xzf "$dist/$archive" -C "$work"
root="$work/${archive%.tar.gz}"
cd "$root"

fail() { echo "server_smoke: FAIL: $*" >&2; echo "--- server stdout"; cat server.out 2>/dev/null || true; echo "--- logs/server.log"; cat logs/server.log 2>/dev/null || true; exit 1; }

token_alpha="$(./bin/aether_server --build-id "$build_id" --issue-token acct-alpha --token-ttl 300)" || fail "--issue-token"
token_bravo="$(./bin/aether_server --build-id "$build_id" --issue-token acct-bravo --token-ttl 300)" || fail "--issue-token"
forged="$(AETHER_SERVER_TOKEN_SECRET=not-the-real-issuer-secret ./bin/aether_server --build-id "$build_id" --issue-token acct-eve)"
case "$token_alpha" in aet1.*) ;; *) fail "unexpected token '$token_alpha'";; esac

AETHER_SERVER_PASSWORD="$password" ./bin/aether_server --port "$port" --build-id "$build_id" --max-runtime 60 \
    --quit-when-empty --status-interval 1 > server.out 2>&1 &
server=$!
for _ in $(seq 1 100); do
    grep -q "^AETHER_SERVER_READY" server.out && break
    kill -0 "$server" 2>/dev/null || fail "the server exited before it was ready"
    sleep 0.1
done
grep -q "^AETHER_SERVER_READY port=$port" server.out || fail "the server never said it was ready"

run_client() {
    AETHER_SERVER_PASSWORD="$password" ./bin/aether_netclient --server "127.0.0.1:$port" --build-id "$build_id" \
        --name "$1" --token "$2" --duration 3 > "client-$1.out" 2>&1
}
run_client alpha "$token_alpha" & c1=$!
run_client bravo "$token_bravo" & c2=$!
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
python3 - client-alpha.out client-bravo.out <<'PY' || fail "client summaries"
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
test -s logs/server.log || fail "no server log"
echo "server_smoke: OK"
