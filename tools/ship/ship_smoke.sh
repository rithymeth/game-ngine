#!/usr/bin/env bash
# M8.10: install, launch, patch, rollback and a refused corrupt patch, on a machine that only has
# python3 and the player's system libraries (CI: bare ubuntu:24.04 + python3 + libvulkan1).
#
#   tools/ship/ship_smoke.sh <dist dir from build_release.sh> <work dir>
#
# Asserts: archive SHA-256 vs SHA256SUMS.txt; every installed file vs GAME_MANIFEST.json; the
# stripped player's build-id matches the symbols archive; v1 launches headless outside any checkout
# and reports Health 3; the patch moves it to v2 (Health 5) with v1 kept as backup; a save in the
# user dir and a user file inside the install survive patch and rollback; rollback restores v1
# (Health 3); a patch with a tampered payload or a damaged archive is refused and v1 stays intact.
set -euo pipefail
dist="$(cd "$1" && pwd)" work="$2"
ship="$(cd "$(dirname "$0")" && pwd)/aether_ship.py"
py="python3 -I $ship"
rm -rf "$work" && mkdir -p "$work" && work="$(cd "$work" && pwd)"
game="$work/opt/AETHER-01" user="$work/user"
step() { echo; echo "== $*"; }
health() {  # launches the installed game headless from an unrelated folder; prints Health
    (cd / && "$game/AETHER-01" --headless --frames 30 --report --user-dir "$user") > "$work/run.log" 2>&1 \
        || { cat "$work/run.log"; echo "ship_smoke: launch failed" >&2; exit 1; }
    sed -n 's/.*"Health":\([0-9.]*\).*/\1/p' "$work/run.log"
}
expect_health() { h="$(health)"; echo "Health $h"; [ "$h" = "$1" ] || { cat "$work/run.log"; echo "ship_smoke: expected Health $1, got '$h'" >&2; exit 1; }; }

step "install v1"
(cd "$dist/v1" && sha256sum -c SHA256SUMS.txt)
$py install --archive "$dist/v1/AETHER-01-linux-x64.tar.gz" --sums "$dist/v1/SHA256SUMS.txt" --dest "$game"
$py verify --install "$game"
$py check-symbols --install "$game" --symbols "$dist/v1/AETHER-01-linux-x64-symbols.tar.gz"
step "launch v1"
expect_health 3.0
mkdir -p "$user/Saved" "$game/Saved"
echo "slot-1 progress" > "$user/Saved/slot1.sav"
echo "in-install user file" > "$game/Saved/local.sav"

step "refuse a damaged patch archive"
mkdir -p "$work/damaged" && cp "$dist/AETHER-01-linux-x64-patch.tar.gz" "$work/damaged/"
python3 -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0xFF; open(p,'wb').write(b)" "$work/damaged/AETHER-01-linux-x64-patch.tar.gz"
if $py apply --install "$game" --patch "$work/damaged/AETHER-01-linux-x64-patch.tar.gz" --sums "$dist/SHA256SUMS.txt"; then echo "ship_smoke: damaged patch applied" >&2; exit 1; fi
step "refuse a patch whose payload does not match its manifest (archive re-packed, no checksum)"
python3 -I - "$dist/AETHER-01-linux-x64-patch.tar.gz" "$work/tampered.tar.gz" <<'PY'
import io, sys, tarfile
src, dst = sys.argv[1:]
with tarfile.open(src, "r:gz") as t, tarfile.open(dst, "w:gz") as o:
    for m in t.getmembers():
        data = t.extractfile(m).read() if m.isfile() else None
        if m.isfile() and m.name.startswith("files/Paks/"):
            data = data[:-1] + bytes([data[-1] ^ 1])
        o.addfile(m, io.BytesIO(data) if data is not None else None)
PY
if $py apply --install "$game" --patch "$work/tampered.tar.gz"; then echo "ship_smoke: tampered patch applied" >&2; exit 1; fi
test ! -e "$game.staging" && test ! -e "$game.previous"
$py verify --install "$game"
expect_health 3.0

step "patch v1 -> v2"
(cd "$dist" && sha256sum -c SHA256SUMS.txt)
$py apply --install "$game" --patch "$dist/AETHER-01-linux-x64-patch.tar.gz" --sums "$dist/SHA256SUMS.txt"
$py verify --install "$game"
$py verify --install "$game.previous"
test -f "$game/Paks/Game_p1.apak"
expect_health 5.0
grep -q "slot-1 progress" "$user/Saved/slot1.sav"
grep -q "in-install user file" "$game/Saved/local.sav"
echo "made on v2" > "$game/Saved/v2.sav"
step "re-applying the same patch is refused (installed build is not the patch's base)"
if $py apply --install "$game" --patch "$dist/AETHER-01-linux-x64-patch.tar.gz"; then echo "ship_smoke: patch re-applied" >&2; exit 1; fi

step "rollback v2 -> v1"
$py rollback --install "$game"
$py verify --install "$game"
test ! -e "$game/Paks/Game_p1.apak"
expect_health 3.0
grep -q "slot-1 progress" "$user/Saved/slot1.sav"
grep -q "in-install user file" "$game/Saved/local.sav"
grep -q "made on v2" "$game/Saved/v2.sav"
step "roll forward again (rollback swaps the backup back in)"
$py rollback --install "$game"
expect_health 5.0
$py rollback --install "$game"
expect_health 3.0
echo; echo "ship_smoke: OK"
