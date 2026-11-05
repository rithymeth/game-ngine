#!/usr/bin/env bash
# M8.10: adds an AETHER-01 game package, its symbols and release manifest to a release
# assets folder and its SHA256SUMS.txt. The Release job runs it on the tagged build (Linux and
# Windows packages); the game-ship jobs run it on every CI build into an empty folder as a dry run.
#
#   tools/ship/assemble_release.sh <package dir from build_release.sh (v1)> <release assets dir> [archive base]
#
# archive base defaults to AETHER-01-linux-x64 (AETHER-01-macos-arm64 for the macOS bundle); the package and symbols names (.tar.gz or .zip)
# are read from <base>.manifest.json.
# Each input is checked against the SHA256SUMS.txt it was packaged with and the manifest's own
# package/symbols hashes; SHA256SUMS.txt is normalised to LF without a BOM, then re-checked whole.
set -euo pipefail
command -v sha256sum >/dev/null 2>&1 || sha256sum() { shasum -a 256 "$@"; }  # stock macOS
pkg="$1" assets="$2" base="${3:-AETHER-01-linux-x64}"
mkdir -p "$assets"
(cd "$pkg" && sha256sum -c SHA256SUMS.txt)
python3 -I - "$pkg" "$base" <<'PY'
import hashlib, json, os, sys
d, base = sys.argv[1:]
m = json.load(open(os.path.join(d, base + ".manifest.json")))
for key in ("package", "symbols_archive"):
    a = m[key]
    have = hashlib.sha256(open(os.path.join(d, a["name"]), "rb").read()).hexdigest()
    assert have == a["sha256"], f"{a['name']}: manifest says {a['sha256']}, file is {have}"
man = m["manifest"]
assert man["symbols"] and all(s["build_id"] for s in man["symbols"]), "manifest lists no symbols"
print(f"{man['project']} {man['version']} build {man['build_id']} rev {man['git_rev']}: "
      f"{len(man['files'])} files, paks {man['paks']}, symbols {[s['build_id'] for s in man['symbols']]}")
PY
names="$(python3 -I -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["package"]["name"], m["symbols_archive"]["name"])' "$pkg/$base.manifest.json") $base.manifest.json"
sums="$assets/SHA256SUMS.txt"
{ [ -f "$sums" ] && tr -d '\r' < "$sums" | sed '1s/^\xEF\xBB\xBF//' | grep -v -F -e "${names// / -e }" || true; } > "$assets/.sums"
for n in $names; do
    cp "$pkg/$n" "$assets/$n"
    (cd "$assets" && sha256sum "$n") >> "$assets/.sums"
done
mv "$assets/.sums" "$sums"
(cd "$assets" && sha256sum -c SHA256SUMS.txt)
