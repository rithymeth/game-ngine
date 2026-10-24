#!/usr/bin/env bash
# M8.10: builds the AETHER-01 Linux release candidate and its first patch from an existing build dir.
#
#   tools/ship/build_release.sh --build <build dir> --out <dist dir> [--version <v>] [--work <dir>]
#
# Writes into <dist>/v1 and <dist>/v2 (each: AETHER-01-linux-x64.tar.gz, -symbols.tar.gz,
# .manifest.json, SHA256SUMS.txt) and <dist>/AETHER-01-linux-x64-patch.tar.gz (+ SHA256SUMS.txt).
# v1 is a strict shipping cook of games/AETHER-01. v2 is v1 plus a delta pak (aether_cook --patch-of,
# Paks/Game_p1.apak) built from a copy of the project whose ThirdPersonController starts the player at
# Health 5 instead of 3; that number is the content marker tools/ship/ship_smoke.sh checks via --report.
set -euo pipefail
build="" out="" version="" work=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "build_release.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$build" ] && [ -n "$out" ] || { sed -n '2,10p' "$0" >&2; exit 2; }
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$(cd "$build" && pwd)"
[ -n "$version" ] || version="$(sed -n 's/.*kEngineVersion = "\([^"]*\)".*/\1/p' "$repo/engine/include/aether/core/version.h")"
[ -n "$work" ] || work="$build/ship-work"
rev="$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$repo" log -1 --format=%ct 2>/dev/null || echo 0)}"
cook="$build/tools/cook/aether_cook"
rm -rf "$work" "$out" && mkdir -p "$work" "$out"

cp -r "$repo/games/AETHER-01" "$work/v1" && rm -rf "$work/v1/Intermediate"
"$cook" "$work/v1/AETHER-01.aproject" --out "$work/cook-v1" --config shipping --strict
cp -r "$work/v1" "$work/v2"
script="$work/v2/Content/Scripts/ThirdPersonController.luau"
grep -q 'Attributes.Define(self.entity, "Health", self.health, 0, 3)' "$script"
sed -i.bak 's/Attributes.Define(self.entity, "Health", self.health, 0, 3)/Attributes.Define(self.entity, "Health", 5, 0, 5)/' "$script" && rm -f "$script.bak"
"$cook" "$work/v2/AETHER-01.aproject" --out "$work/cook-v2" --config shipping --strict \
    --patch-of "$work/cook-v1/Game.apak" --pak-name Game_p1

py=python3
"$py" "$here/aether_ship.py" package --build "$build" --cooked "$work/cook-v1" --version "$version" \
    --git-rev "$rev" --out "$out/v1"
"$py" "$here/aether_ship.py" package --build "$build" --cooked "$work/cook-v1" --extra-pak "$work/cook-v2/Game_p1.apak" \
    --version "$version+p1" --git-rev "$rev" --out "$out/v2"
"$py" "$here/aether_ship.py" make-patch --from "$out/v1/AETHER-01-linux-x64.tar.gz" \
    --to "$out/v2/AETHER-01-linux-x64.tar.gz" --out "$out/AETHER-01-linux-x64-patch.tar.gz"
