#!/usr/bin/env bash
# M8.10: install, launch, patch, rollback and a refused corrupt patch, on a machine that only has
# python3 and the player's system libraries (CI: bare ubuntu:24.04 + python3 + libvulkan1; and the
# Windows runner under Git Bash with PYTHON=python).
#
#   tools/ship/ship_smoke.sh <dist dir from build_release.sh> <work dir> [platform] [archive base]
#
# platform defaults to linux-x64 (archives .tar.gz, launcher AETHER-01); windows-x64 uses .zip and
# AETHER-01.exe; macos-arm64 uses .tar.gz and the app bundle's AETHER-01.app/Contents/MacOS/AETHER-01
# (paks in AETHER-01.app/Contents/Resources/Paks). archive base defaults to AETHER-01-<platform>.
#
# Asserts: archive SHA-256 vs SHA256SUMS.txt; every installed file vs GAME_MANIFEST.json; the
# stripped player's build-id matches the symbols archive; v1 launches headless outside any checkout
# and reports Health 3; the patch moves it to v2 (Health 5) with v1 kept as backup; a save in the
# user dir and a user file inside the install survive patch and rollback; rollback restores v1
# (Health 3); a patch with a tampered payload or a damaged archive is refused and v1 stays intact.
set -euo pipefail
dist="$(cd "$1" && pwd)" work="$2" platform="${3:-linux-x64}"
base="${4:-AETHER-01-$platform}"
ship="$(cd "$(dirname "$0")" && pwd)/aether_ship.py"
python="${PYTHON:-python3}"
py="$python -I $ship"
paks=Paks
case "$platform" in
    windows*) ext=zip launcher=AETHER-01.exe ;;
    macos*) ext=tar.gz launcher=AETHER-01.app/Contents/MacOS/AETHER-01 paks=AETHER-01.app/Contents/Resources/Paks ;;
    *) ext=tar.gz launcher=AETHER-01 ;;
esac
command -v sha256sum >/dev/null 2>&1 || sha256sum() { shasum -a 256 "$@"; }  # stock macOS
pkg="$base.$ext" syms="$base-symbols.$ext" patch="$base-patch.$ext"
rm -rf "$work" && mkdir -p "$work" && work="$(cd "$work" && pwd)"
game="$work/opt/AETHER-01" user="$work/user"
step() { echo; echo "== $*"; }
health() {  # launches the installed game headless from an unrelated folder; prints Health
    (cd / && "$game/$launcher" --headless --frames 30 --report --user-dir "$user") > "$work/run.log" 2>&1 \
        || { cat "$work/run.log"; echo "ship_smoke: launch failed" >&2; exit 1; }
    sed -n 's/.*"Health":\([0-9.]*\).*/\1/p' "$work/run.log"
}
expect_health() { h="$(health)"; echo "Health $h"; [ "$h" = "$1" ] || { cat "$work/run.log"; echo "ship_smoke: expected Health $1, got '$h'" >&2; exit 1; }; }

step "install v1"
(cd "$dist/v1" && sha256sum -c SHA256SUMS.txt)
$py install --archive "$dist/v1/$pkg" --sums "$dist/v1/SHA256SUMS.txt" --dest "$game"
$py verify --install "$game"
$py check-symbols --install "$game" --symbols "$dist/v1/$syms"
step "launch v1"
expect_health 3.0
mkdir -p "$user/Saved" "$game/Saved"
echo "slot-1 progress" > "$user/Saved/slot1.sav"
echo "in-install user file" > "$game/Saved/local.sav"

step "refuse a damaged patch archive"
mkdir -p "$work/damaged" && cp "$dist/$patch" "$work/damaged/"
$python -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0xFF; open(p,'wb').write(b)" "$work/damaged/$patch"
if $py apply --install "$game" --patch "$work/damaged/$patch" --sums "$dist/SHA256SUMS.txt"; then echo "ship_smoke: damaged patch applied" >&2; exit 1; fi
step "refuse a patch whose payload does not match its manifest (archive re-packed, no checksum)"
$python -I - "$dist/$patch" "$work/tampered.$ext" <<'PY'
import io, sys, tarfile, zipfile
src, dst = sys.argv[1:]
flip = lambda name, data: data[:-1] + bytes([data[-1] ^ 1]) if name.startswith("files/") and name.endswith(".apak") and data else data
if zipfile.is_zipfile(src):
    with zipfile.ZipFile(src) as z, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as o:
        for i in z.infolist():
            o.writestr(i, flip(i.filename, z.read(i)))
else:
    with tarfile.open(src, "r:gz") as t, tarfile.open(dst, "w:gz") as o:
        for m in t.getmembers():
            data = flip(m.name, t.extractfile(m).read()) if m.isfile() else None
            o.addfile(m, io.BytesIO(data) if data is not None else None)
PY
if $py apply --install "$game" --patch "$work/tampered.$ext"; then echo "ship_smoke: tampered patch applied" >&2; exit 1; fi
test ! -e "$game.staging" && test ! -e "$game.previous"
$py verify --install "$game"
expect_health 3.0

step "patch v1 -> v2"
(cd "$dist" && sha256sum -c SHA256SUMS.txt)
$py apply --install "$game" --patch "$dist/$patch" --sums "$dist/SHA256SUMS.txt"
$py verify --install "$game"
$py verify --install "$game.previous"
test -f "$game/$paks/Game_p1.apak"
expect_health 5.0
grep -q "slot-1 progress" "$user/Saved/slot1.sav"
grep -q "in-install user file" "$game/Saved/local.sav"
echo "made on v2" > "$game/Saved/v2.sav"
step "re-applying the same patch is refused (installed build is not the patch's base)"
if $py apply --install "$game" --patch "$dist/$patch"; then echo "ship_smoke: patch re-applied" >&2; exit 1; fi

step "rollback v2 -> v1"
$py rollback --install "$game"
$py verify --install "$game"
test ! -e "$game/$paks/Game_p1.apak"
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
