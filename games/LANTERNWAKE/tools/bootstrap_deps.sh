#!/usr/bin/env bash
set -euo pipefail

lanternwake_tools_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
lanternwake_game_dir="$(cd -- "$lanternwake_tools_dir/.." && pwd)"
lanternwake_engine_dir="$(cd -- "$lanternwake_game_dir/../.." && pwd)"
lanternwake_deps_dir="${LANTERNWAKE_DEPS_DIR:-$lanternwake_engine_dir/build/deps}"
mkdir -p -- "$lanternwake_deps_dir"

fetch_pinned() {
    local repository="$1" revision="$2" directory="$3"
    local created=false
    if [[ ! -d "$directory/.git" ]]; then
        if [[ -e "$directory" ]]; then
            printf 'Dependency directory is not a git checkout: %s\n' "$directory" >&2
            return 1
        fi
        git clone --filter=blob:none --no-checkout "$repository" "$directory"
        created=true
    fi
    local current
    current="$(git -C "$directory" rev-parse HEAD 2>/dev/null || true)"
    if [[ "$created" == true || "$current" != "$revision" ]]; then
        if [[ "$created" == false && -n "$(git -C "$directory" status --porcelain)" ]]; then
            printf 'Dependency checkout has local changes; leaving it intact: %s\n' "$directory" >&2
            return 1
        fi
        if ! git -C "$directory" cat-file -e "$revision^{commit}" 2>/dev/null; then
            git -C "$directory" fetch --depth 1 origin "$revision"
        fi
        git -C "$directory" -c advice.detachedHead=false checkout --detach "$revision"
    fi
}

# Official upstreams, with fixed revisions. stb matches the engine root's pin.
fetch_pinned https://github.com/nothings/stb.git \
    2c980bb59875b0d32144a71867fbdebb2f77cd20 "$lanternwake_deps_dir/stb"
# libsdl-org/SDL release-2.32.10.
fetch_pinned https://github.com/libsdl-org/SDL.git \
    5d249570393f7a37e037abf22cd6012a4cc56a71 "$lanternwake_deps_dir/SDL2"

printf 'Official dependencies ready in %s\n' "$lanternwake_deps_dir"
