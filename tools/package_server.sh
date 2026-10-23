#!/usr/bin/env bash
# M8.9: packages the dedicated server - aether_server, aether_netclient, a
# server.json and a cooked game's .apak archives - into
#   <out>/aether-server-<platform>.tar.gz   (one top-level folder of the same name)
#   <out>/SHA256SUMS.txt                    (sha256sum format, like the other release artifacts)
#   <out>/SERVER_PACKAGE.json               (name, bytes, sha256 of the archive and every file in it)
#
#   tools/package_server.sh --build <build dir> --paks <cooked dir> --out <dir>
#                           [--platform linux-x64] [--config <server.json>] [--no-strip]
#
# The cooked dir is aether_cook's --out (its *.apak are copied to Paks/). The
# package runs from anywhere: bin/aether_server finds ../server.json and ../Paks.
set -euo pipefail

build="" paks="" out="" platform="linux-x64" config="" strip_bins=1
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --paks) paks="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --platform) platform="$2"; shift 2 ;;
        --config) config="$2"; shift 2 ;;
        --no-strip) strip_bins=0; shift ;;
        *) echo "package_server.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$build" ] && [ -n "$paks" ] && [ -n "$out" ] || { sed -n '2,13p' "$0" >&2; exit 2; }
here="$(cd "$(dirname "$0")" && pwd)"
[ -n "$config" ] || config="$here/server/server.json"

for exe in aether_server aether_netclient; do
    [ -x "$build/tools/server/$exe" ] || { echo "package_server.sh: $build/tools/server/$exe is missing (build it first)" >&2; exit 1; }
done
shopt -s nullglob
apaks=("$paks"/*.apak)
[ ${#apaks[@]} -gt 0 ] || { echo "package_server.sh: no .apak in $paks" >&2; exit 1; }

name="aether-server-$platform"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$out/$name"
rm -rf "$stage" "$out/$name.tar.gz"
mkdir -p "$stage/bin" "$stage/Paks" "$stage/logs"
cp "$build/tools/server/aether_server" "$build/tools/server/aether_netclient" "$stage/bin/"
if [ "$strip_bins" = 1 ] && command -v strip > /dev/null; then strip "$stage/bin/aether_server" "$stage/bin/aether_netclient"; fi
cp "${apaks[@]}" "$stage/Paks/"
cp "$config" "$stage/server.json"
cp "$here/server/README.txt" "$stage/README.txt"

# Deterministic archive: sorted names, fixed owner and mtime.
mtime="@${SOURCE_DATE_EPOCH:-$(git -C "$here/.." log -1 --format=%ct 2>/dev/null || echo 0)}"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="$mtime" -C "$out" -cf - "$name" | gzip -n -9 > "$out/$name.tar.gz"

(cd "$out" && sha256sum "$name.tar.gz" > SHA256SUMS.txt)
python3 - "$out" "$name" <<'PY'
import hashlib, json, os, sys
out, name = sys.argv[1], sys.argv[2]
def digest(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()
stage = os.path.join(out, name)
files = []
for root, _, names in os.walk(stage):
    for n in sorted(names):
        p = os.path.join(root, n)
        files.append({"path": os.path.relpath(p, stage), "bytes": os.path.getsize(p), "sha256": digest(p)})
archive = os.path.join(out, name + ".tar.gz")
manifest = {"name": name + ".tar.gz", "bytes": os.path.getsize(archive), "sha256": digest(archive),
            "files": sorted(files, key=lambda f: f["path"])}
with open(os.path.join(out, "SERVER_PACKAGE.json"), "w") as f: json.dump(manifest, f, indent=2)
PY
rm -rf "$stage"
cat "$out/SHA256SUMS.txt"
