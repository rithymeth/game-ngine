#!/usr/bin/env python3
"""M8.10 Build & Ship: game release packages, manifests, symbols, patches and rollback.

Standard library only, so the install/patch/rollback half runs on a clean
machine that has nothing but python3 (CI uses a bare ubuntu:24.04 container).

  aether_ship.py package  --build <dir> --cooked <dir> --version <v> --out <dir>
                          [--project-dir games/AETHER-01] [--name AETHER-01] [--platform linux-x64]
                          [--extra-pak <file.apak>]... [--git-rev <sha>] [--no-symbols]
      Stages bin/aether_player (stripped, with its debug info split into a
      build-id keyed symbols archive), a launcher, Paks/, notices and
      GAME_MANIFEST.json, then writes into <out>:
        <name>-<platform>.tar.gz             the package (one top folder <name>/)
        <name>-<platform>-symbols.tar.gz     .build-id/xx/yyyy.debug + SYMBOLS.json
        <name>-<platform>.manifest.json      the release manifest (package + archive hashes)
        SHA256SUMS.txt                       sha256sum format
      Output is byte-identical for identical inputs (sorted entries, fixed
      owner, mtime = SOURCE_DATE_EPOCH or 0, no timestamps in the manifest).

  aether_ship.py make-patch --from <v1 package.tar.gz> --to <v2 package.tar.gz> --out <patch.tar.gz>
      Writes only the files that differ (plus PATCH.json and the v2 manifest).

  aether_ship.py install  --archive <pkg.tar.gz> --dest <empty dir> [--sums SHA256SUMS.txt]
  aether_ship.py verify   --install <dir>
  aether_ship.py apply    --install <dir> --patch <patch.tar.gz> [--sums SHA256SUMS.txt]
  aether_ship.py rollback --install <dir>
  aether_ship.py check-symbols --install <dir> --symbols <symbols.tar.gz>

Windows (--platform windows-x64): the package and symbols are .zip files, the
player is staged as <name>.exe at the package root (it finds Paks/ next to
itself) with the player's DLLs, and there is no ELF build-id. Symbols are the
.pdb files the PE CodeView (RSDS) records name, in a symbol-store layout
(<pdb>/<GUID><age>/<pdb>) with SYMBOLS.json; check-symbols compares each
binary's RSDS GUID/age with the GUID (PDB info stream) and age (DBI stream)
read from the .pdb itself. A patch is a .zip when --out ends in .zip.
Switching uses os.replace on folders (no links); if a file in the install is
in use (the game is running) the switch fails with a clear error and the
install is left as it was or recoverable.

Install layout: <dir> is the live install, <dir>.previous the backup kept by
apply (rollback swaps the two, so a rolled-back update can be re-applied with
rollback again). Files in <dir> that its manifest does not list (for example a
Saved/ folder) are user files: they are carried across apply and rollback.
Saves written by the player go to the user dir (outside the install) anyway.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import uuid
import zipfile
import zlib

MANIFEST = "GAME_MANIFEST.json"
PATCH = "PATCH.json"
MANIFEST_TYPE = "AetherGameRelease"
PATCH_TYPE = "AetherGamePatch"
FORMAT_VERSION = 1
MAX_JSON = 16 * 1024 * 1024


class ShipError(Exception):
    pass


# ---------------------------------------------------------------- helpers

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj) -> bytes:
    return (json.dumps(obj, indent=2, sort_keys=True) + "\n").encode("utf-8")


def parse_version(text: str) -> tuple:
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)", text or "")
    if not m:
        raise ShipError(f"not a version: {text!r}")
    return tuple(int(x) for x in m.groups())


def safe_relpath(rel: str) -> str:
    """A manifest/archive path: relative, '/'-separated, no '..', no drive or stream syntax."""
    if not isinstance(rel, str) or not rel or "\\" in rel or ":" in rel or "\0" in rel:
        raise ShipError(f"bad path {rel!r}")
    if rel.startswith("/") or any(p in ("", ".", "..") for p in rel.split("/")):
        raise ShipError(f"bad path {rel!r}")
    return rel


def read_json_file(path: str):
    if os.path.islink(path) or not os.path.isfile(path):
        raise ShipError(f"{path}: missing or not a regular file")
    if os.path.getsize(path) > MAX_JSON:
        raise ShipError(f"{path}: too large")
    with open(path, "rb") as f:
        try:
            return json.loads(f.read().decode("utf-8"))
        except ValueError as e:
            raise ShipError(f"{path}: invalid JSON ({e})")


def read_sums(path: str) -> dict:
    sums = {}
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                sums[parts[1].lstrip("*")] = parts[0].lower()
    return sums


def check_archive_sum(archive: str, sums_file: str | None) -> None:
    if not sums_file:
        return
    want = read_sums(sums_file).get(os.path.basename(archive))
    have = sha256_file(archive)
    if want != have:
        raise ShipError(f"{os.path.basename(archive)}: SHA256SUMS says {want}, file is {have}")


# ------------------------------------------------------------- ELF build-id

def elf_build_id(path: str) -> str | None:
    """The NT_GNU_BUILD_ID note of an ELF file (section or program headers), hex; None if absent."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        return None
    is64, little = data[4] == 2, data[5] == 1
    e = "<" if little else ">"

    def notes(off, size):
        end = off + size
        while off + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from(e + "III", data, off)
            name_off = off + 12
            desc_off = name_off + ((namesz + 3) & ~3)
            if ntype == 3 and data[name_off:name_off + namesz].rstrip(b"\0") == b"GNU":
                return data[desc_off:desc_off + descsz].hex()
            off = desc_off + ((descsz + 3) & ~3)
        return None

    if is64:
        shoff, = struct.unpack_from(e + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 0x3A)
        phoff, = struct.unpack_from(e + "Q", data, 0x20)
        phentsize, phnum = struct.unpack_from(e + "HH", data, 0x36)
    else:
        shoff, = struct.unpack_from(e + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 0x2E)
        phoff, = struct.unpack_from(e + "I", data, 0x1C)
        phentsize, phnum = struct.unpack_from(e + "HH", data, 0x2C)
    for i in range(shnum):  # SHT_NOTE sections (present in .debug files, which have no usable segments)
        base = shoff + i * shentsize
        sh_type, = struct.unpack_from(e + "I", data, base + 4)
        if sh_type != 7:
            continue
        if is64:
            off, size = struct.unpack_from(e + "QQ", data, base + 0x18)
        else:
            off, size = struct.unpack_from(e + "II", data, base + 0x10)
        found = notes(off, size) if off + size <= len(data) else None
        if found:
            return found
    for i in range(phnum):  # PT_NOTE
        base = phoff + i * phentsize
        p_type, = struct.unpack_from(e + "I", data, base)
        if p_type != 4:
            continue
        if is64:
            off, = struct.unpack_from(e + "Q", data, base + 8)
            size, = struct.unpack_from(e + "Q", data, base + 0x20)
        else:
            off, = struct.unpack_from(e + "I", data, base + 4)
            size, = struct.unpack_from(e + "I", data, base + 0x10)
        found = notes(off, size)
        if found:
            return found
    return None


def elf_needed(path: str) -> list:
    """DT_NEEDED libraries, via readelf (build host only); [] when readelf is unavailable."""
    try:
        out = subprocess.run(["readelf", "-d", path], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return []
    return sorted(set(re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)))


# ------------------------------------------------------------ PE / PDB

def _pe_layout(data: bytes):
    """(data directories [(rva, size)], sections [(va, vsize, raw_ptr, raw_size)]) of a PE image; None if not PE."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    pe, = struct.unpack_from("<I", data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    if magic == 0x20B:
        ndirs_off, dirs_off = opt + 108, opt + 112
    elif magic == 0x10B:
        ndirs_off, dirs_off = opt + 92, opt + 96
    else:
        return None
    ndirs, = struct.unpack_from("<I", data, ndirs_off)
    dirs = [struct.unpack_from("<II", data, dirs_off + 8 * i) for i in range(min(ndirs, 16))]
    secs = []
    sec = opt + opt_size
    for i in range(nsec):
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        secs.append((va, vsize, raw_ptr, raw_size))
    return dirs, secs


def _rva_to_off(secs, rva: int):
    for va, vsize, raw_ptr, raw_size in secs:
        if va <= rva < va + max(vsize, raw_size):
            return raw_ptr + (rva - va)
    return None


def format_guid(raw: bytes) -> str:
    return str(uuid.UUID(bytes_le=bytes(raw))).upper()


def symstore_key(guid: str, age: int) -> str:
    """The symbol-server folder for a PDB: GUID without dashes + age in hex."""
    return guid.replace("-", "").upper() + format(age, "X")


def pe_codeview(path: str):
    """The RSDS CodeView record of a PE file: {'guid', 'age', 'pdb'}; None if not PE or no RSDS record."""
    with open(path, "rb") as f:
        data = f.read()
    try:
        layout = _pe_layout(data)
        if not layout:
            return None
        dirs, secs = layout
        if len(dirs) <= 6 or not dirs[6][0]:
            return None
        rva, size = dirs[6]
        off = _rva_to_off(secs, rva)
        if off is None:
            return None
        for i in range(size // 28):
            dtype, dsize, _addr, ptr = struct.unpack_from("<IIII", data, off + 28 * i + 12)
            if dtype == 2 and dsize >= 24 and data[ptr:ptr + 4] == b"RSDS":  # IMAGE_DEBUG_TYPE_CODEVIEW
                guid = format_guid(data[ptr + 4:ptr + 20])
                age, = struct.unpack_from("<I", data, ptr + 20)
                name = data[ptr + 24:ptr + dsize].split(b"\0", 1)[0].decode("utf-8", "replace")
                return {"guid": guid, "age": age, "pdb": name}
    except struct.error:
        return None
    return None


def pe_imports(path: str) -> list:
    """Imported DLL names of a PE file (import directory), sorted; [] if not PE."""
    with open(path, "rb") as f:
        data = f.read()
    names = set()
    try:
        layout = _pe_layout(data)
        if not layout or len(layout[0]) <= 1 or not layout[0][1][0]:
            return []
        dirs, secs = layout
        off = _rva_to_off(secs, dirs[1][0])
        while off is not None and off + 20 <= len(data):
            name_rva, = struct.unpack_from("<I", data, off + 12)
            if not name_rva:
                break
            noff = _rva_to_off(secs, name_rva)
            if noff is not None:
                names.add(data[noff:data.index(b"\0", noff)].decode("ascii", "replace"))
            off += 20
    except (struct.error, ValueError):
        pass
    return sorted(names, key=str.lower)


PDB_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


def pdb_info(path: str):
    """{'guid', 'age'} of an MSF 7.00 PDB: GUID from the PDB info stream (1), age from the DBI stream (3)
    when present (that is the age debuggers match), else from the info stream. None if not a PDB."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(PDB_MAGIC):
        return None
    try:
        block, _fpm, nblocks, dir_bytes, _unk, map_addr = struct.unpack_from("<6I", data, 32)
        if block not in (512, 1024, 2048, 4096) or nblocks * block > len(data) + block:
            return None
        ndir_blocks = (dir_bytes + block - 1) // block
        dir_blocks = struct.unpack_from("<%dI" % ndir_blocks, data, map_addr * block)
        directory = b"".join(data[b * block:(b + 1) * block] for b in dir_blocks)[:dir_bytes]
        nstreams, = struct.unpack_from("<I", directory, 0)
        sizes = [0 if n == 0xFFFFFFFF else n for n in struct.unpack_from("<%dI" % nstreams, directory, 4)]
        firsts, pos = [], 4 + 4 * nstreams  # each stream's block list follows the size table, in order
        for sz in sizes:
            firsts.append(pos)
            pos += 4 * ((sz + block - 1) // block)

        def stream(i):
            n = (sizes[i] + block - 1) // block
            blocks = struct.unpack_from("<%dI" % n, directory, firsts[i])
            return b"".join(data[b * block:(b + 1) * block] for b in blocks)[:sizes[i]]

        if nstreams < 2:
            return None
        info = stream(1)
        _ver, _sig, age = struct.unpack_from("<III", info, 0)
        guid = format_guid(info[12:28])
        if nstreams > 3:
            dbi = stream(3)
            if len(dbi) >= 12:
                age, = struct.unpack_from("<I", dbi, 8)
        return {"guid": guid, "age": age}
    except struct.error:
        return None


def is_windows(platform: str) -> bool:
    return platform.startswith("windows")


def player_path(m: dict) -> str:
    """The player binary inside an install: <launcher> (the exe itself) on Windows, bin/aether_player elsewhere."""
    return m["launcher"] if is_windows(m.get("platform", "")) and m.get("launcher") else "bin/aether_player"


# --------------------------------------------------------------- manifest

def file_entries(root: str) -> list:
    entries = []
    for dirpath, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel == MANIFEST:
                continue
            if os.path.islink(full):
                raise ShipError(f"{rel}: links are not allowed in a package")
            entries.append({"path": rel, "bytes": os.path.getsize(full), "sha256": sha256_file(full)})
    entries.sort(key=lambda x: x["path"])
    return entries


def build_manifest(root: str, meta: dict) -> dict:
    files = file_entries(root)
    paks = [f["path"] for f in files if f["path"].startswith("Paks/") and f["path"].endswith(".apak")]
    # Content identity: what is installed, independent of when or where it was built.
    build_id = sha256_bytes(b"".join(f"{f['path']}\0{f['sha256']}\n".encode() for f in files))[:16]
    manifest = {
        "$type": MANIFEST_TYPE,
        "$version": FORMAT_VERSION,
        "project": meta["project"],
        "version": meta["version"],
        "platform": meta["platform"],
        "build_id": build_id,
        "git_rev": meta.get("git_rev", ""),
        "engine_version": meta["engine_version"],
        "min_engine_version": meta.get("min_engine_version", meta["engine_version"]),
        "launcher": meta.get("launcher", ""),
        "toolchain": meta.get("toolchain", {}),
        "runtime_dependencies": meta.get("runtime_dependencies", []),
        "licenses": meta.get("licenses", []),
        "paks": paks,  # mount order: the player sorts Paks/*.apak by name, later ones win
        "symbols": meta.get("symbols", []),
        "files": files,
    }
    validate_manifest(manifest)
    return manifest


def validate_manifest(m) -> None:
    if not isinstance(m, dict) or m.get("$type") != MANIFEST_TYPE or m.get("$version") != FORMAT_VERSION:
        raise ShipError(f"not a {MANIFEST_TYPE} v{FORMAT_VERSION} manifest")
    for key in ("project", "version", "platform", "build_id", "engine_version", "min_engine_version"):
        if not isinstance(m.get(key), str) or not m[key]:
            raise ShipError(f"manifest: missing {key}")
    parse_version(m["engine_version"])
    parse_version(m["min_engine_version"])
    files = m.get("files")
    if not isinstance(files, list) or not files:
        raise ShipError("manifest lists no files")
    seen = set()
    for f in files:
        if not isinstance(f, dict) or not isinstance(f.get("bytes"), int) or f["bytes"] < 0 \
                or not re.fullmatch(r"[0-9a-f]{64}", str(f.get("sha256", ""))):
            raise ShipError(f"manifest: bad file entry {f!r}")
        rel = safe_relpath(f.get("path"))
        if rel == MANIFEST or rel.lower() in seen:
            raise ShipError(f"manifest: repeated or reserved path {rel}")
        seen.add(rel.lower())
    for p in m.get("paks", []):
        if safe_relpath(p).lower() not in seen:
            raise ShipError(f"manifest: pak {p} is not a listed file")


def verify_tree(root: str, manifest: dict) -> list:
    """Problems with <root> against <manifest> (missing, changed, linked files); [] when it matches."""
    validate_manifest(manifest)
    problems = []
    for f in manifest["files"]:
        full = os.path.join(root, *f["path"].split("/"))
        parent = root
        linked = False
        for part in f["path"].split("/")[:-1]:
            parent = os.path.join(parent, part)
            if os.path.islink(parent):
                linked = True
        if linked or os.path.islink(full):
            problems.append(f"{f['path']}: linked")
        elif not os.path.isfile(full):
            problems.append(f"{f['path']}: missing")
        elif os.path.getsize(full) != f["bytes"]:
            problems.append(f"{f['path']}: {os.path.getsize(full)} bytes, manifest says {f['bytes']}")
        elif sha256_file(full) != f["sha256"]:
            problems.append(f"{f['path']}: sha256 differs from the manifest")
    return problems


def load_installed(root: str) -> dict:
    m = read_json_file(os.path.join(root, MANIFEST))
    validate_manifest(m)
    return m


def verify_install(root: str) -> dict:
    m = load_installed(root)
    problems = verify_tree(root, m)
    if problems:
        raise ShipError(f"{root} does not match its manifest:\n  " + "\n  ".join(problems))
    return m


# ---------------------------------------------------------------- archives

def write_tar_gz(archive: str, root: str, top: str, mtime: int, executable=lambda rel: False) -> None:
    """Deterministic .tar.gz of <root> under folder <top>/ (or at the archive root when top is '')."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as t:
        items = []
        for dirpath, dirs, names in os.walk(root):
            dirs.sort()
            rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
            if rel_dir != ".":
                items.append((rel_dir, None))
            for n in sorted(names):
                rel = n if rel_dir == "." else rel_dir + "/" + n
                items.append((rel, os.path.join(dirpath, n)))
        if top:
            items.append(("", None))
        for rel, full in sorted(items):
            name = (top + "/" + rel).rstrip("/") if top else rel
            info = tarfile.TarInfo(name)
            info.mtime, info.uid, info.gid, info.uname, info.gname = mtime, 0, 0, "", ""
            if full is None:
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                t.addfile(info)
            else:
                info.size = os.path.getsize(full)
                info.mode = 0o755 if executable(rel) else 0o644
                with open(full, "rb") as f:
                    t.addfile(info, f)
    with open(archive, "wb") as f, gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as g:
        g.write(raw.getvalue())


def write_zip(archive: str, root: str, top: str, mtime: int) -> None:
    """Deterministic .zip of <root> under folder <top>/ (or at the archive root when top is '')."""
    import time
    stamp = time.gmtime(max(mtime, 315532800))[:6]  # zip dates start at 1980
    items = []
    for dirpath, dirs, names in os.walk(root):
        dirs.sort()
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir != ".":
            items.append((rel_dir + "/", None))
        for n in sorted(names):
            rel = n if rel_dir == "." else rel_dir + "/" + n
            items.append((rel, os.path.join(dirpath, n)))
    if top:
        items.append(("", None))
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel, full in sorted(items):
            name = (top + "/" + rel) if top else rel
            if not name:
                continue
            info = zipfile.ZipInfo(name, stamp)
            info.create_system = 0  # MS-DOS attributes: no Unix modes or owners recorded
            if full is None:
                info.external_attr = 0x10
                z.writestr(info, b"")
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0x20
                with open(full, "rb") as f:
                    z.writestr(info, f.read())


def write_archive(archive: str, root: str, top: str, mtime: int, executable=lambda rel: False) -> None:
    if archive.lower().endswith(".zip"):
        write_zip(archive, root, top, mtime)
    else:
        write_tar_gz(archive, root, top, mtime, executable)


def _safe_extract_zip(archive: str, dest: str) -> None:
    try:
        with zipfile.ZipFile(archive) as z:
            infos = z.infolist()
            for i in infos:
                safe_relpath(i.filename.rstrip("/"))
                mode = i.external_attr >> 16
                if i.create_system == 3 and mode and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
                    raise ShipError(f"{archive}: {i.filename} is not a regular file or folder")
                if i.flag_bits & 0x1:
                    raise ShipError(f"{archive}: {i.filename} is encrypted")
            for i in infos:
                target = os.path.join(dest, *i.filename.rstrip("/").split("/"))
                if i.is_dir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(i) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, OSError, EOFError, zlib.error, NotImplementedError) as e:
        raise ShipError(f"{archive}: unreadable archive ({e})")


def safe_extract(archive: str, dest: str) -> None:
    """Extracts regular files and folders only; refuses links, devices, absolute or '..' paths."""
    try:
        with open(archive, "rb") as f:
            is_zip = f.read(4) == b"PK\x03\x04"
    except OSError as e:
        raise ShipError(f"{archive}: unreadable archive ({e})")
    if is_zip:
        _safe_extract_zip(archive, dest)
        return
    try:
        with tarfile.open(archive, "r:gz") as t:
            members = t.getmembers()
            for m in members:
                safe_relpath(m.name.rstrip("/"))
                if not (m.isfile() or m.isdir()):
                    raise ShipError(f"{archive}: {m.name} is not a regular file or folder")
            for m in members:
                target = os.path.join(dest, *m.name.rstrip("/").split("/"))
                if m.isdir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                src = t.extractfile(m)
                with open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
                os.chmod(target, 0o755 if m.mode & 0o111 else 0o644)
    except (tarfile.TarError, OSError, EOFError, zlib.error) as e:
        raise ShipError(f"{archive}: unreadable archive ({e})")


def single_top(dir_: str) -> str:
    entries = os.listdir(dir_)
    if len(entries) != 1 or not os.path.isdir(os.path.join(dir_, entries[0])):
        raise ShipError("package archive must hold exactly one top-level folder")
    return os.path.join(dir_, entries[0])


# ---------------------------------------------------------------- package

def read_engine_version(repo: str) -> str:
    with open(os.path.join(repo, "engine/include/aether/core/version.h"), encoding="utf-8") as f:
        m = re.search(r'kEngineVersion\s*=\s*"([^"]+)"', f.read())
    if not m:
        raise ShipError("cannot read kEngineVersion")
    return m.group(1)


def read_toolchain(build: str) -> dict:
    tc = {}
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.isfile(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            for line in f:
                for key in ("CMAKE_BUILD_TYPE", "CMAKE_GENERATOR"):
                    if line.startswith(key + ":"):
                        tc[key.lower().replace("cmake_", "")] = line.split("=", 1)[1].strip()
    files = os.path.join(build, "CMakeFiles")
    if os.path.isdir(files):
        for ver in sorted(os.listdir(files)):
            cxx = os.path.join(files, ver, "CMakeCXXCompiler.cmake")
            if os.path.isfile(cxx):
                text = open(cxx, encoding="utf-8", errors="replace").read()
                tc["cmake"] = ver
                for key, name in (("CMAKE_CXX_COMPILER_ID", "compiler"), ("CMAKE_CXX_COMPILER_VERSION", "compiler_version")):
                    m = re.search(r'set\(%s "([^"]*)"\)' % key, text)
                    if m:
                        tc[name] = m.group(1)
    return tc


LAUNCHER = """#!/bin/sh
# Starts the packaged game from wherever it was installed. Extra arguments go to the player
# (for example --headless --frames 60 --report, or --user-dir <dir> for saves and settings).
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/bin/aether_player" --pak "$here/Paks" "$@"
"""


def find_pdb(binary_src: str, cv: dict, search: list):
    """The .pdb a CodeView record names: its recorded path, else <name> in the binary's folder or <search>."""
    name = re.split(r"[\\/]", cv["pdb"])[-1]
    cands = [cv["pdb"]] + [os.path.join(d, name) for d in [os.path.dirname(binary_src)] + search]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def stage_windows(a, stage: str, symdir: str) -> tuple:
    """Copies <name>.exe (the player) and its DLLs into <stage>; returns (player src, symbols, runtime deps)."""
    player = None
    for cand in ("player/Release/aether_player.exe", "player/aether_player.exe"):
        if os.path.isfile(os.path.join(a.build, cand)):
            player = os.path.join(a.build, cand)
            break
    if not player:
        raise ShipError(f"aether_player.exe not found in {a.build}/player")
    player_dir = os.path.dirname(player)
    binaries = [(player, a.name + ".exe")]
    for n in sorted(os.listdir(player_dir)):
        if n.lower().endswith(".dll"):
            binaries.append((os.path.join(player_dir, n), n))
    symbols = []
    deps = set()
    for src, rel in binaries:
        dst = os.path.join(stage, rel)
        shutil.copyfile(src, dst)
        deps.update(pe_imports(dst))
        if a.no_symbols:
            continue
        cv = pe_codeview(dst)
        pdb = find_pdb(src, cv, [player_dir]) if cv else None
        if not pdb:
            if src == player:
                raise ShipError(f"{player}: no CodeView record or .pdb found (link with /DEBUG; compile with /Zi)")
            print(f"aether_ship: {rel}: no .pdb found, shipped without symbols", file=sys.stderr)
            continue
        info = pdb_info(pdb)
        if not info or info["guid"] != cv["guid"] or info["age"] != cv["age"]:
            raise ShipError(f"{pdb} does not match {rel} (exe {cv['guid']}/{cv['age']}, pdb {info})")
        pdb_name = os.path.basename(pdb)
        key = symstore_key(cv["guid"], cv["age"])
        debug_rel = f"{pdb_name}/{key}/{pdb_name}"
        debug = os.path.join(symdir, *debug_rel.split("/"))
        os.makedirs(os.path.dirname(debug), exist_ok=True)
        shutil.copyfile(pdb, debug)
        symbols.append({"binary": rel, "build_id": key, "guid": cv["guid"], "age": cv["age"], "pdb": pdb_name,
                        "debug_file": debug_rel, "bytes": os.path.getsize(debug), "sha256": sha256_file(debug)})
    shipped = {r.lower() for _, r in binaries}
    system = sorted(d for d in deps if d.lower() not in shipped)
    return player, symbols, system


def cmd_package(a) -> None:
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    windows = is_windows(a.platform)
    project_dir = a.project_dir or os.path.join(repo, "games", a.name)
    player = None
    for cand in ("player/Release/aether_player", "player/aether_player"):
        if not windows and os.path.isfile(os.path.join(a.build, cand)):
            player = os.path.join(a.build, cand)
            break
    if not player and not windows:
        raise ShipError(f"aether_player not found in {a.build}/player")
    paks = sorted(os.path.join(a.cooked, n) for n in os.listdir(a.cooked) if n.endswith(".apak")) + list(a.extra_pak or [])
    if not paks:
        raise ShipError(f"no .apak in {a.cooked}")
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    pkg_name = a.archive_name or f"{a.name}-{a.platform}"
    ext = ".zip" if windows else ".tar.gz"
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        stage = os.path.join(tmp, a.name)
        symdir = os.path.join(tmp, "symbols")
        os.makedirs(os.path.join(stage, "Paks"))
        symbols = []
        if windows:
            os.makedirs(symdir)
            player, symbols, runtime_deps = stage_windows(a, stage, symdir)
        else:
            os.makedirs(os.path.join(stage, "bin"))
            bin_player = os.path.join(stage, "bin", "aether_player")
            shutil.copyfile(player, bin_player)
            os.chmod(bin_player, 0o755)
        if not a.no_symbols and not windows:
            build_id = elf_build_id(bin_player)
            if not build_id:
                raise ShipError(f"{player} has no GNU build-id (link with -Wl,--build-id)")
            debug_rel = f".build-id/{build_id[:2]}/{build_id[2:]}.debug"
            debug = os.path.join(symdir, *debug_rel.split("/"))
            os.makedirs(os.path.dirname(debug))
            subprocess.run(["objcopy", "--only-keep-debug", "--compress-debug-sections", bin_player, debug], check=True)
            subprocess.run(["objcopy", "--strip-debug", "--strip-unneeded", "--remove-section=.gnu_debuglink",
                            f"--add-gnu-debuglink={debug}", bin_player], check=True)
            if elf_build_id(debug) != build_id or elf_build_id(bin_player) != build_id:
                raise ShipError("build-id changed while splitting symbols")
            os.chmod(debug, 0o644)
            symbols.append({"binary": "bin/aether_player", "build_id": build_id, "debug_file": debug_rel,
                            "bytes": os.path.getsize(debug), "sha256": sha256_file(debug)})
        for p in paks:
            shutil.copyfile(p, os.path.join(stage, "Paks", os.path.basename(p)))
        cook_manifest = os.path.join(a.cooked, "CookManifest.json")
        if os.path.isfile(cook_manifest):
            shutil.copyfile(cook_manifest, os.path.join(stage, "CookManifest.json"))
        if not windows:
            with open(os.path.join(stage, a.name), "w", newline="\n") as f:
                f.write(LAUNCHER)
            runtime_deps = elf_needed(bin_player)
        else:
            reqs = os.path.join(repo, "docs", "WINDOWS_REQUIREMENTS.md")
            if os.path.isfile(reqs):
                shutil.copyfile(reqs, os.path.join(stage, "WINDOWS_REQUIREMENTS.md"))
        for src, dst in (("README.md", "README.md"), ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md")):
            if os.path.isfile(os.path.join(project_dir, src)):
                shutil.copyfile(os.path.join(project_dir, src), os.path.join(stage, dst))
        shutil.copyfile(os.path.join(repo, "THIRD_PARTY_NOTICES.md"), os.path.join(stage, "ENGINE_NOTICES.md"))
        shutil.copytree(os.path.join(repo, "third_party", "licenses"), os.path.join(stage, "licenses"))
        engine_version = read_engine_version(repo)
        meta = {
            "project": a.name, "version": a.version, "platform": a.platform, "git_rev": a.git_rev,
            "engine_version": engine_version, "min_engine_version": engine_version,
            "launcher": a.name + ".exe" if windows else a.name, "toolchain": read_toolchain(a.build),
            "runtime_dependencies": runtime_deps,
            "licenses": sorted(os.listdir(os.path.join(stage, "licenses"))),
            "symbols": symbols,
        }
        manifest = build_manifest(stage, meta)
        manifest_bytes = canonical_json(manifest)
        with open(os.path.join(stage, MANIFEST), "wb") as f:
            f.write(manifest_bytes)
        if verify_tree(stage, manifest):
            raise ShipError("staged package does not match its own manifest")
        archive = os.path.join(out, pkg_name + ext)
        execs = {"bin/aether_player", a.name}
        write_archive(archive, stage, a.name, mtime, lambda rel: rel in execs)
        outputs = [archive]
        release = {"$type": "AetherGameReleaseAsset", "$version": FORMAT_VERSION,
                   "package": {"name": os.path.basename(archive), "bytes": os.path.getsize(archive),
                               "sha256": sha256_file(archive)},
                   "manifest_sha256": sha256_bytes(manifest_bytes), "manifest": manifest}
        if symbols:
            with open(os.path.join(symdir, "SYMBOLS.json"), "wb") as f:
                f.write(canonical_json({"project": a.name, "version": a.version, "build_id": manifest["build_id"],
                                        "symbols": symbols}))
            sym_archive = os.path.join(out, pkg_name + "-symbols" + ext)
            write_archive(sym_archive, symdir, "", mtime)
            outputs.append(sym_archive)
            release["symbols_archive"] = {"name": os.path.basename(sym_archive), "bytes": os.path.getsize(sym_archive),
                                          "sha256": sha256_file(sym_archive)}
    asset_manifest = os.path.join(out, pkg_name + ".manifest.json")
    with open(asset_manifest, "wb") as f:
        f.write(canonical_json(release))
    outputs.append(asset_manifest)
    append_sums(out, outputs)
    for o in outputs:
        print(f"{sha256_file(o)}  {os.path.basename(o)}")


def append_sums(out: str, files: list) -> None:
    path = os.path.join(out, "SHA256SUMS.txt")
    sums = read_sums(path) if os.path.isfile(path) else {}
    for f in files:
        sums[os.path.basename(f)] = sha256_file(f)
    with open(path, "w", newline="\n") as f:
        for name in sorted(sums):
            f.write(f"{sums[name]}  {name}\n")


# ------------------------------------------------------------------ patch

def load_package_archive(archive: str, tmp: str) -> tuple:
    dest = os.path.join(tmp, "pkg-" + sha256_bytes(archive.encode())[:8])
    os.makedirs(dest)
    safe_extract(archive, dest)
    root = single_top(dest)
    return root, verify_install(root)


def cmd_make_patch(a) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        old_root, old = load_package_archive(a.from_, tmp)
        new_root, new = load_package_archive(a.to, tmp)
        if old["project"] != new["project"] or old["platform"] != new["platform"]:
            raise ShipError("patch must be between packages of the same project and platform")
        old_files = {f["path"]: f for f in old["files"]}
        new_files = {f["path"]: f for f in new["files"]}
        changed = [f for p, f in sorted(new_files.items()) if old_files.get(p, {}).get("sha256") != f["sha256"]]
        removed = sorted(p for p in old_files if p not in new_files)
        stage = os.path.join(tmp, "patch")
        os.makedirs(os.path.join(stage, "files"))
        for f in changed:
            dst = os.path.join(stage, "files", *f["path"].split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(new_root, *f["path"].split("/")), dst)
        manifest_bytes = canonical_json(new)
        with open(os.path.join(stage, MANIFEST), "wb") as f:
            f.write(manifest_bytes)
        patch = {"$type": PATCH_TYPE, "$version": FORMAT_VERSION, "project": new["project"], "platform": new["platform"],
                 "from": {"version": old["version"], "build_id": old["build_id"]},
                 "to": {"version": new["version"], "build_id": new["build_id"], "manifest_sha256": sha256_bytes(manifest_bytes)},
                 "files": changed, "removed": removed}
        with open(os.path.join(stage, PATCH), "wb") as f:
            f.write(canonical_json(patch))
        out = os.path.abspath(a.out)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        write_archive(out, stage, "", int(os.environ.get("SOURCE_DATE_EPOCH", "0")),
                      lambda rel: rel.startswith("files/") and new_files.get(rel[6:], {}).get("path") in ("bin/aether_player", new["launcher"]))
    append_sums(os.path.dirname(out), [out])
    print(f"{old['version']} -> {new['version']}: {len(changed)} changed, {len(removed)} removed, "
          f"{os.path.getsize(out)} bytes ({os.path.basename(out)})")


def read_patch(patch_dir: str) -> tuple:
    patch = read_json_file(os.path.join(patch_dir, PATCH))
    if not isinstance(patch, dict) or patch.get("$type") != PATCH_TYPE or patch.get("$version") != FORMAT_VERSION:
        raise ShipError(f"not a {PATCH_TYPE} v{FORMAT_VERSION}")
    manifest_path = os.path.join(patch_dir, MANIFEST)
    new = read_json_file(manifest_path)
    validate_manifest(new)
    if sha256_file(manifest_path) != patch.get("to", {}).get("manifest_sha256") or new["build_id"] != patch["to"].get("build_id"):
        raise ShipError("patch manifest does not match PATCH.json")
    new_files = {f["path"]: f for f in new["files"]}
    for f in patch.get("files", []):
        rel = safe_relpath(f.get("path"))
        want = new_files.get(rel)
        if not want or want["sha256"] != f.get("sha256") or want["bytes"] != f.get("bytes"):
            raise ShipError(f"patch file {rel} is not in the target manifest")
        full = os.path.join(patch_dir, "files", *rel.split("/"))
        if os.path.islink(full) or not os.path.isfile(full) or os.path.getsize(full) != want["bytes"] or sha256_file(full) != want["sha256"]:
            raise ShipError(f"patch file {rel} is corrupt (size/sha256 differ from the manifest)")
    for rel in patch.get("removed", []):
        safe_relpath(rel)
        if rel in new_files:
            raise ShipError(f"patch removes {rel} but the target manifest lists it")
    return patch, new


def user_files(root: str, manifest: dict) -> list:
    """Files under <root> the manifest does not list (and that are not the manifest): saves, settings, logs."""
    listed = {f["path"] for f in manifest["files"]} | {MANIFEST}
    found = []
    for dirpath, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(dirpath, n), root).replace(os.sep, "/")
            if rel not in listed:
                found.append(rel)
    return found


IN_USE_HINT = "a file in it is in use or locked (close the game and anything reading its folder, then retry)"


def _rmtree(path: str) -> None:
    """shutil.rmtree that also clears read-only attributes (Windows); ShipError when files are in use."""
    def onerror(fn, p, _exc):
        try:
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
            fn(p)
        except OSError as e:
            raise ShipError(f"cannot remove {path}: {IN_USE_HINT} ({e})")
    shutil.rmtree(path, onerror=onerror)


def _move(src: str, dst: str) -> None:
    """One folder rename (os.replace; dst must not exist). Fails clearly when Windows refuses (files in use)."""
    try:
        os.replace(src, dst)
    except OSError as e:
        raise ShipError(f"cannot rename {src} -> {dst}: {IN_USE_HINT} ({e})")


def recover(install: str) -> None:
    """Finishes a switch interrupted between its two renames: <install> gone, a verified staging folder present."""
    staging = install + ".staging"
    if not os.path.exists(install) and os.path.isdir(staging):
        verify_install(staging)
        _move(staging, install)
        print(f"recovered an interrupted update: {staging} -> {install}")


def switch(install: str, new_root: str) -> None:
    """<install> becomes <install>.previous, <new_root> becomes <install>. Each step is one rename.
    If the first rename fails (Windows: the game is running) nothing changed but the staging copy, which is
    removed. If the second fails, <install> is missing and the verified staging folder is what recover() uses."""
    previous = install + ".previous"
    staging = install + ".staging"
    if new_root != staging:
        _move(new_root, staging)
    try:
        if os.path.exists(previous):
            _rmtree(previous)
        _move(install, previous)
    except ShipError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    _move(staging, install)


def cmd_apply(a) -> None:
    install = os.path.abspath(a.install).rstrip(os.sep)
    recover(install)
    check_archive_sum(a.patch, a.sums)
    current = verify_install(install)
    staging = install + ".staging"
    if os.path.exists(staging):
        _rmtree(staging)
    with tempfile.TemporaryDirectory(dir=os.path.dirname(install)) as tmp:
        safe_extract(a.patch, tmp)
        patch, new = read_patch(tmp)
        if patch.get("project") != current["project"] or patch.get("platform") != current["platform"]:
            raise ShipError("patch is for a different project or platform")
        if patch["from"].get("build_id") != current["build_id"]:
            raise ShipError(f"patch is from {patch['from'].get('version')} ({patch['from'].get('build_id')}); "
                            f"installed is {current['version']} ({current['build_id']})")
        replaces_player = any(f["path"] == player_path(new) for f in patch["files"])
        if not replaces_player and parse_version(current["engine_version"]) < parse_version(new["min_engine_version"]):
            raise ShipError(f"patch needs engine {new['min_engine_version']}, installed player is {current['engine_version']}")
        # Stage a full copy (user files included), apply, verify the result against the target manifest.
        shutil.copytree(install, staging, symlinks=True)
        try:
            for rel in patch["removed"]:
                p = os.path.join(staging, *rel.split("/"))
                if os.path.isfile(p):
                    os.remove(p)
            for f in patch["files"]:
                dst = os.path.join(staging, *f["path"].split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst):
                    os.remove(dst)
                shutil.copyfile(os.path.join(tmp, "files", *f["path"].split("/")), dst)
                if f["path"] in (player_path(new), new["launcher"]):
                    os.chmod(dst, 0o755)
            shutil.copyfile(os.path.join(tmp, MANIFEST), os.path.join(staging, MANIFEST))
            problems = verify_tree(staging, new)
            if problems:
                raise ShipError("patched copy does not match the target manifest:\n  " + "\n  ".join(problems))
        except BaseException:
            shutil.rmtree(staging, ignore_errors=True)
            raise
    switch(install, staging)
    print(f"updated {current['project']} {current['version']} -> {new['version']} "
          f"({len(patch['files'])} files, {len(patch['removed'])} removed); backup: {install}.previous")


def cmd_rollback(a) -> None:
    install = os.path.abspath(a.install).rstrip(os.sep)
    recover(install)
    previous = install + ".previous"
    if not os.path.isdir(previous):
        raise ShipError(f"no backup to roll back to ({previous})")
    target = verify_install(previous)
    current = load_installed(install)
    staging = install + ".staging"
    if os.path.exists(staging):
        _rmtree(staging)
    shutil.copytree(previous, staging, symlinks=True)
    try:
        # Carry user files (saves, settings) the update created over to the version being restored.
        for rel in user_files(install, current):
            src = os.path.join(install, *rel.split("/"))
            dst = os.path.join(staging, *rel.split("/"))
            if any(f["path"] == rel for f in target["files"]):
                continue  # never overwrite a file the restored version owns
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        problems = verify_tree(staging, target)
        if problems:
            raise ShipError("restored copy does not match its manifest:\n  " + "\n  ".join(problems))
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    switch(install, staging)
    print(f"rolled back {current['project']} {current['version']} -> {target['version']}; "
          f"{current['version']} kept in {previous}")


def cmd_install(a) -> None:
    dest = os.path.abspath(a.dest)
    if os.path.exists(dest) and os.listdir(dest):
        raise ShipError(f"{dest} is not empty")
    check_archive_sum(a.archive, a.sums)
    parent = os.path.dirname(dest)
    os.makedirs(parent, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent) as tmp:
        safe_extract(a.archive, tmp)
        root = single_top(tmp)
        m = verify_install(root)
        if os.path.exists(dest):
            os.rmdir(dest)
        _move(root, dest)
    print(f"installed {m['project']} {m['version']} ({m['build_id']}, {len(m['files'])} files) into {dest}")


def cmd_verify(a) -> None:
    m = verify_install(os.path.abspath(a.install))
    print(f"{m['project']} {m['version']} build {m['build_id']}: {len(m['files'])} files match {MANIFEST}")


def cmd_check_symbols(a) -> None:
    install = os.path.abspath(a.install)
    m = verify_install(install)
    with tempfile.TemporaryDirectory() as tmp:
        safe_extract(a.symbols, tmp)
        index = read_json_file(os.path.join(tmp, "SYMBOLS.json"))
        if index.get("build_id") != m["build_id"]:
            raise ShipError("symbols archive is for a different build")
        if not m["symbols"]:
            raise ShipError("manifest lists no symbols")
        for s in m["symbols"]:
            binary = os.path.join(install, *safe_relpath(s["binary"]).split("/"))
            debug = os.path.join(tmp, *safe_relpath(s["debug_file"]).split("/"))
            if "guid" in s:  # Windows: PE CodeView RSDS record vs the PDB's own GUID/age
                cv = pe_codeview(binary)
                info = pdb_info(debug) if os.path.isfile(debug) else None
                want = (s["guid"], s["age"])
                if not cv or not info or (cv["guid"], cv["age"]) != want or (info["guid"], info["age"]) != want:
                    raise ShipError(f"{s['binary']}: RSDS {cv}, pdb {info}, manifest {want}")
                if sha256_file(debug) != s["sha256"]:
                    raise ShipError(f"{s['debug_file']}: sha256 differs from the manifest")
                print(f"{s['binary']}: PDB GUID {cv['guid']} age {cv['age']} matches {s['debug_file']}")
                continue
            have_bin, have_dbg = elf_build_id(binary), elf_build_id(debug) if os.path.isfile(debug) else None
            if not (have_bin == have_dbg == s["build_id"]):
                raise ShipError(f"{s['binary']}: build-id {have_bin}, symbols {have_dbg}, manifest {s['build_id']}")
            if sha256_file(debug) != s["sha256"]:
                raise ShipError(f"{s['debug_file']}: sha256 differs from the manifest")
            print(f"{s['binary']}: build-id {have_bin} matches {s['debug_file']}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("package")
    s.add_argument("--build", required=True)
    s.add_argument("--cooked", required=True)
    s.add_argument("--version", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--name", default="AETHER-01")
    s.add_argument("--project-dir")
    s.add_argument("--platform", default="linux-x64")
    s.add_argument("--extra-pak", action="append")
    s.add_argument("--git-rev", default="")
    s.add_argument("--no-symbols", action="store_true")
    s.add_argument("--archive-name", help="output base name (default <name>-<platform>)")
    s.set_defaults(fn=cmd_package)
    s = sub.add_parser("make-patch")
    s.add_argument("--from", dest="from_", required=True)
    s.add_argument("--to", required=True)
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_make_patch)
    s = sub.add_parser("install")
    s.add_argument("--archive", required=True)
    s.add_argument("--dest", required=True)
    s.add_argument("--sums")
    s.set_defaults(fn=cmd_install)
    s = sub.add_parser("verify")
    s.add_argument("--install", required=True)
    s.set_defaults(fn=cmd_verify)
    s = sub.add_parser("apply")
    s.add_argument("--install", required=True)
    s.add_argument("--patch", required=True)
    s.add_argument("--sums")
    s.set_defaults(fn=cmd_apply)
    s = sub.add_parser("rollback")
    s.add_argument("--install", required=True)
    s.set_defaults(fn=cmd_rollback)
    s = sub.add_parser("check-symbols")
    s.add_argument("--install", required=True)
    s.add_argument("--symbols", required=True)
    s.set_defaults(fn=cmd_check_symbols)
    a = p.parse_args(argv)
    try:
        a.fn(a)
    except ShipError as e:
        print(f"aether_ship: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
