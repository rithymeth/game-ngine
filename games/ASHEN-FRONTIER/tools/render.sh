#!/bin/bash
# Cooks the project and renders one frame of a scene with the real player (a software Vulkan device is enough):
#   tools/render.sh Scenes/ModelGallery.ascene out.png [frames] [WxH]
# Needs the engine built (build/ at the repository root, or AETHER_BUILD=<dir>).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${AETHER_BUILD:-$HERE/../../build}"
SCENE="${1:-Scenes/FallenBasin.ascene}"
OUT="${2:-render.png}"
FRAMES="${3:-30}"
SIZE="${4:-1280x720}"
WORK="$(mktemp -d)"
"$BUILD/tools/cook/aether_cook" "$HERE/ASHEN-FRONTIER.aproject" --out "$WORK/cooked" --always "$SCENE" > "$WORK/cook.log" 2>&1 || { cat "$WORK/cook.log"; exit 1; }
xvfb-run -a "$BUILD/player/aether_player" --pak "$WORK/cooked" --scene "$SCENE" --frames "$FRAMES" --size "$SIZE" --screenshot "$WORK/shot.bmp" --user-dir "$WORK/user" > "$WORK/player.log" 2>&1 || { tail -20 "$WORK/player.log"; exit 1; }
convert "$WORK/shot.bmp" "$OUT"
echo "wrote $OUT"
rm -rf "$WORK"
