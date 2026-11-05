#!/usr/bin/env bash
# M8.10: builds the AETHER-01 release candidate and its first patch from an existing build dir.
#
#   tools/ship/build_release.sh --build <build dir> --out <dist dir> [--version <v>] [--work <dir>]
#                               [--platform linux-x64|windows-x64|macos-arm64] [--archive-name <base>]
#
# Writes into <dist>/v1 and <dist>/v2 (each: <base>.tar.gz, -symbols.tar.gz, .manifest.json,
# SHA256SUMS.txt) and <dist>/<base>-patch.tar.gz (+ SHA256SUMS.txt); <base> defaults to
# AETHER-01-<platform>. For windows-x64 (Git Bash, MSVC build dir with .pdb files) the archives are .zip;
# macos-arm64 (RelWithDebInfo build, Xcode command-line tools) packages an AETHER-01.app bundle + dSYM.
# v1 is a strict shipping cook of games/AETHER-01. v2 is v1 plus a delta pak (aether_cook --patch-of,
# Paks/Game_p1.apak) built from a copy of the project whose ThirdPersonController starts the player at
# Health 5 instead of 3; that number is the content marker tools/ship/ship_smoke.sh checks via --report.
set -euo pipefail
build="" out="" version="" work="" platform="linux-x64" base=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --platform) platform="$2"; shift 2 ;;
        --archive-name) base="$2"; shift 2 ;;
        *) echo "build_release.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$build" ] && [ -n "$out" ] || { sed -n '2,13p' "$0" >&2; exit 2; }
[ -n "$base" ] || base="AETHER-01-$platform"
case "$platform" in windows*) ext=zip ;; *) ext=tar.gz ;; esac
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$(cd "$build" && pwd)"
[ -n "$version" ] || version="$(sed -n 's/.*kEngineVersion = "\([^"]*\)".*/\1/p' "$repo/engine/include/aether/core/version.h")"
[ -n "$work" ] || work="$build/ship-work"
rev="$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$repo" log -1 --format=%ct 2>/dev/null || echo 0)}"
cook="$build/tools/cook/aether_cook"
for c in "$build/tools/cook/Release/aether_cook.exe" "$build/tools/cook/aether_cook.exe" "$build/tools/cook/Release/aether_cook"; do
    [ -x "$cook" ] || cook="$c"
done
rm -rf "$work" "$out" && mkdir -p "$work" "$out"

cp -r "$repo/games/AETHER-01" "$work/v1" && rm -rf "$work/v1/Intermediate"
"$cook" "$work/v1/AETHER-01.aproject" --out "$work/cook-v1" --config shipping --strict
cp -r "$work/v1" "$work/v2"
script="$work/v2/Content/Scripts/ThirdPersonController.luau"
grep -q 'Attributes.Define(self.entity, "Health", self.health, 0, 3)' "$script"
sed -i.bak 's/Attributes.Define(self.entity, "Health", self.health, 0, 3)/Attributes.Define(self.entity, "Health", 5, 0, 5)/' "$script" && rm -f "$script.bak"
"$cook" "$work/v2/AETHER-01.aproject" --out "$work/cook-v2" --config shipping --strict \
    --patch-of "$work/cook-v1/Game.apak" --pak-name Game_p1

py="${PYTHON:-python3}"
ship=("$py" "$here/aether_ship.py")
pkg=(--platform "$platform" --archive-name "$base")
"${ship[@]}" package --build "$build" --cooked "$work/cook-v1" --version "$version" \
    --git-rev "$rev" --out "$out/v1" "${pkg[@]}"
"${ship[@]}" package --build "$build" --cooked "$work/cook-v1" --extra-pak "$work/cook-v2/Game_p1.apak" \
    --version "$version+p1" --git-rev "$rev" --out "$out/v2" "${pkg[@]}"
"${ship[@]}" make-patch --from "$out/v1/$base.$ext" \
    --to "$out/v2/$base.$ext" --out "$out/$base-patch.$ext"
