#!/usr/bin/env bash
# M8.10: adds the AETHER-01 Linux game package, its symbols and release manifest to a release
# assets folder and its SHA256SUMS.txt. The Release job runs it on the tagged build; the
# game-ship job runs it on every CI build into an empty folder as a dry run (nothing is published).
#
#   tools/ship/assemble_release.sh <package dir from build_release.sh (v1)> <release assets dir>
#
# Each input is checked against the SHA256SUMS.txt it was packaged with and the manifest's own
# package/symbols hashes; SHA256SUMS.txt is normalised to LF without a BOM, then re-checked whole.
set -euo pipefail
pkg="$1" assets="$2"
mkdir -p "$assets"
(cd "$pkg" && sha256sum -c SHA256SUMS.txt)
names="AETHER-01-linux-x64.tar.gz AETHER-01-linux-x64-symbols.tar.gz AETHER-01-linux-x64.manifest.json"
python3 -I - "$pkg" <<'PY'
import hashlib, json, os, sys
d = sys.argv[1]
m = json.load(open(os.path.join(d, "AETHER-01-linux-x64.manifest.json")))
for key in ("package", "symbols_archive"):
    a = m[key]
    have = hashlib.sha256(open(os.path.join(d, a["name"]), "rb").read()).hexdigest()
    assert have == a["sha256"], f"{a['name']}: manifest says {a['sha256']}, file is {have}"
man = m["manifest"]
assert man["symbols"] and all(s["build_id"] for s in man["symbols"]), "manifest lists no symbols"
print(f"{man['project']} {man['version']} build {man['build_id']} rev {man['git_rev']}: "
      f"{len(man['files'])} files, paks {man['paks']}, symbols {[s['build_id'] for s in man['symbols']]}")
PY
sums="$assets/SHA256SUMS.txt"
{ [ -f "$sums" ] && tr -d '\r' < "$sums" | sed '1s/^\xEF\xBB\xBF//' | grep -v -F -e "${names// / -e }" || true; } > "$assets/.sums"
for n in $names; do
    cp "$pkg/$n" "$assets/$n"
    (cd "$assets" && sha256sum "$n") >> "$assets/.sums"
done
mv "$assets/.sums" "$sums"
(cd "$assets" && sha256sum -c SHA256SUMS.txt)
