#!/usr/bin/env bash
set -euo pipefail

lanternwake_tools_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
lanternwake_game_dir="$(cd -- "$lanternwake_tools_dir/.." && pwd)"
lanternwake_engine_dir="$(cd -- "$lanternwake_game_dir/../.." && pwd)"
lanternwake_build_dir="${LANTERNWAKE_NATIVE_BUILD:-$lanternwake_engine_dir/build-lanternwake}"
lanternwake_cmake="${CMAKE:-cmake}"
if ! command -v "$lanternwake_cmake" >/dev/null 2>&1; then
    if [[ -x /home/agent/.local/bin/cmake ]]; then lanternwake_cmake=/home/agent/.local/bin/cmake
    else printf 'CMake 3.21 or newer is required.\n' >&2; exit 1; fi
fi

bash "$lanternwake_tools_dir/bootstrap_deps.sh"
lanternwake_generator=()
if command -v ninja >/dev/null 2>&1; then lanternwake_generator=(-G Ninja); fi
"$lanternwake_cmake" -S "$lanternwake_game_dir" -B "$lanternwake_build_dir" \
    "${lanternwake_generator[@]}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DLANTERNWAKE_DEPS_DIR="${LANTERNWAKE_DEPS_DIR:-$lanternwake_engine_dir/build/deps}" "$@"
"$lanternwake_cmake" --build "$lanternwake_build_dir" --config Release --parallel "${LANTERNWAKE_JOBS:-4}"
printf 'Built Lanternwake in %s\n' "$lanternwake_build_dir"
