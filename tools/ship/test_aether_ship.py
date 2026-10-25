#!/usr/bin/env python3
"""Unit tests for tools/ship/aether_ship.py: manifests, install, patch, rollback, symbols (M8.10)."""
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import unittest.mock
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aether_ship as ship  # noqa: E402

META = {"project": "DEMO", "platform": "linux-x64", "engine_version": "0.27.6", "launcher": "DEMO", "git_rev": "abc"}


def make_package(root: str, version: str, files: dict, mtime: int = 0, **meta) -> str:
    """Writes <root>/<version>/DEMO-linux-x64.tar.gz holding <files> (path -> bytes) plus a manifest."""
    stage = os.path.join(root, "stage-" + version, "DEMO")
    for rel, data in files.items():
        p = os.path.join(stage, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    m = ship.build_manifest(stage, dict(META, version=version, **meta))
    with open(os.path.join(stage, ship.MANIFEST), "wb") as f:
        f.write(ship.canonical_json(m))
    out = os.path.join(root, version)
    os.makedirs(out, exist_ok=True)
    archive = os.path.join(out, "DEMO-linux-x64.tar.gz")
    ship.write_tar_gz(archive, stage, "DEMO", mtime)
    ship.append_sums(out, [archive])
    return archive


V1 = {"DEMO": b"#!/bin/sh\n", "bin/aether_player": b"player-v1", "Paks/Game.apak": b"base" * 100, "README.md": b"v1 readme"}
V2 = dict(V1, **{"Paks/Game_p1.apak": b"delta", "README.md": b"v2 readme"})


def run(*argv) -> int:
    return ship.main(list(argv))


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_manifest_lists_files_paks_in_mount_order_and_is_deterministic(self):
        a = make_package(os.path.join(self.tmp, "a"), "1.0.0", V2, mtime=123)
        b = make_package(os.path.join(self.tmp, "b"), "1.0.0", V2, mtime=123)
        self.assertEqual(ship.sha256_file(a), ship.sha256_file(b))
        dest = os.path.join(self.tmp, "inst")
        self.assertEqual(run("install", "--archive", a, "--dest", dest), 0)
        m = ship.verify_install(dest)
        self.assertEqual(m["paks"], ["Paks/Game.apak", "Paks/Game_p1.apak"])
        self.assertEqual([f["path"] for f in m["files"]], sorted(V2))
        self.assertEqual(m["min_engine_version"], "0.27.6")
        self.assertEqual(len(m["build_id"]), 16)
        entry = next(f for f in m["files"] if f["path"] == "README.md")
        self.assertEqual(entry["bytes"], len(V2["README.md"]))

    def test_build_id_depends_on_content(self):
        s1, s2 = os.path.join(self.tmp, "s1"), os.path.join(self.tmp, "s2")
        for s, data in ((s1, b"a"), (s2, b"b")):
            os.makedirs(s)
            open(os.path.join(s, "x"), "wb").write(data)
        m1 = ship.build_manifest(s1, dict(META, version="1.0.0"))
        m2 = ship.build_manifest(s2, dict(META, version="1.0.0"))
        self.assertNotEqual(m1["build_id"], m2["build_id"])

    def test_verify_detects_changed_missing_and_linked_files(self):
        dest = os.path.join(self.tmp, "inst")
        run("install", "--archive", make_package(self.tmp, "1.0.0", V1), "--dest", dest)
        open(os.path.join(dest, "README.md"), "wb").write(b"V1 README")  # same size, other bytes
        os.remove(os.path.join(dest, "Paks", "Game.apak"))
        problems = ship.verify_tree(dest, ship.load_installed(dest))
        self.assertEqual(len(problems), 2, problems)
        self.assertNotEqual(run("verify", "--install", dest), 0)

    def test_manifest_validation_rejects_bad_paths_and_repeats(self):
        good = {"path": "a", "bytes": 1, "sha256": "0" * 64}
        base = {"$type": ship.MANIFEST_TYPE, "$version": 1, "project": "p", "version": "1", "platform": "x",
                "build_id": "b", "engine_version": "0.1.0", "min_engine_version": "0.1.0"}
        ship.validate_manifest(dict(base, files=[good]))
        for bad in ("../x", "/abs", "a/../b", "c:x", "a\\b", "", "a//b", ship.MANIFEST):
            with self.assertRaises(ship.ShipError, msg=bad):
                ship.validate_manifest(dict(base, files=[dict(good, path=bad)]))
        with self.assertRaises(ship.ShipError):
            ship.validate_manifest(dict(base, files=[good, dict(good, path="A")]))
        with self.assertRaises(ship.ShipError):
            ship.validate_manifest(dict(base, files=[dict(good, sha256="xyz")]))
        with self.assertRaises(ship.ShipError):
            ship.validate_manifest(dict(base, files=[good], paks=["Paks/missing.apak"]))

    def test_install_checks_sums_and_refuses_non_empty_destination(self):
        archive = make_package(self.tmp, "1.0.0", V1)
        sums = os.path.join(os.path.dirname(archive), "SHA256SUMS.txt")
        dest = os.path.join(self.tmp, "inst")
        os.makedirs(dest)
        open(os.path.join(dest, "x"), "w").write("x")
        self.assertNotEqual(run("install", "--archive", archive, "--dest", dest, "--sums", sums), 0)
        bad = os.path.join(self.tmp, "bad.txt")
        open(bad, "w").write("0" * 64 + "  DEMO-linux-x64.tar.gz\n")
        self.assertNotEqual(run("install", "--archive", archive, "--dest", os.path.join(self.tmp, "i2"), "--sums", bad), 0)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "i2")))
        self.assertEqual(run("install", "--archive", archive, "--dest", os.path.join(self.tmp, "i3"), "--sums", sums), 0)

    def test_extract_refuses_traversal_and_links(self):
        for member in ("../evil", "/abs/evil"):
            archive = os.path.join(self.tmp, "evil.tar.gz")
            with tarfile.open(archive, "w:gz") as t:
                info = tarfile.TarInfo(member)
                info.size = 1
                t.addfile(info, io.BytesIO(b"x"))
            with self.assertRaises(ship.ShipError):
                ship.safe_extract(archive, os.path.join(self.tmp, "out"))
        with tarfile.open(archive, "w:gz") as t:
            info = tarfile.TarInfo("link")
            info.type, info.linkname = tarfile.SYMTYPE, "/etc/passwd"
            t.addfile(info)
        with self.assertRaises(ship.ShipError):
            ship.safe_extract(archive, os.path.join(self.tmp, "out"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "evil")))


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.v1 = make_package(self.tmp, "1.0.0", V1)
        self.v2 = make_package(self.tmp, "1.0.1", V2)
        self.patch = os.path.join(self.tmp, "dist", "DEMO-linux-x64-patch.tar.gz")
        self.assertEqual(run("make-patch", "--from", self.v1, "--to", self.v2, "--out", self.patch), 0)
        self.sums = os.path.join(self.tmp, "dist", "SHA256SUMS.txt")
        self.game = os.path.join(self.tmp, "opt", "DEMO")
        self.assertEqual(run("install", "--archive", self.v1, "--dest", self.game), 0)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def version(self, path=None):
        return ship.verify_install(path or self.game)["version"]

    def test_patch_holds_only_changed_files(self):
        with tarfile.open(self.patch) as t:
            names = sorted(n for n in t.getnames() if n.startswith("files/") and t.getmember(n).isfile())
            patch = json.load(t.extractfile(ship.PATCH))
        self.assertEqual(names, ["files/Paks/Game_p1.apak", "files/README.md"])
        self.assertEqual(patch["from"]["version"], "1.0.0")
        self.assertEqual(patch["to"]["version"], "1.0.1")
        self.assertEqual(patch["removed"], [])

    def test_apply_then_rollback_keeps_user_files(self):
        os.makedirs(os.path.join(self.game, "Saved"))
        open(os.path.join(self.game, "Saved", "slot.sav"), "w").write("progress")
        self.assertEqual(run("apply", "--install", self.game, "--patch", self.patch, "--sums", self.sums), 0)
        self.assertEqual(self.version(), "1.0.1")
        self.assertEqual(self.version(self.game + ".previous"), "1.0.0")
        self.assertFalse(os.path.exists(self.game + ".staging"))
        self.assertEqual(open(os.path.join(self.game, "README.md"), "rb").read(), b"v2 readme")
        self.assertEqual(open(os.path.join(self.game, "Saved", "slot.sav")).read(), "progress")
        open(os.path.join(self.game, "Saved", "new.sav"), "w").write("made on v2")
        self.assertEqual(run("rollback", "--install", self.game), 0)
        self.assertEqual(self.version(), "1.0.0")
        self.assertFalse(os.path.exists(os.path.join(self.game, "Paks", "Game_p1.apak")))
        self.assertEqual(open(os.path.join(self.game, "Saved", "slot.sav")).read(), "progress")
        self.assertEqual(open(os.path.join(self.game, "Saved", "new.sav")).read(), "made on v2")
        self.assertEqual(self.version(self.game + ".previous"), "1.0.1")
        self.assertEqual(run("rollback", "--install", self.game), 0)  # forward again
        self.assertEqual(self.version(), "1.0.1")

    def test_removed_files_are_deleted(self):
        v3_files = dict(V2)
        del v3_files["README.md"]
        v3 = make_package(os.path.join(self.tmp, "v3"), "1.0.2", v3_files)
        p = os.path.join(self.tmp, "p3", "patch.tar.gz")
        self.assertEqual(run("make-patch", "--from", self.v1, "--to", v3, "--out", p), 0)
        self.assertEqual(run("apply", "--install", self.game, "--patch", p), 0)
        self.assertFalse(os.path.exists(os.path.join(self.game, "README.md")))
        self.assertEqual(self.version(), "1.0.2")

    def assert_refused_and_v1_intact(self, patch, *extra):
        before = ship.sha256_file(os.path.join(self.game, ship.MANIFEST))
        self.assertNotEqual(run("apply", "--install", self.game, "--patch", patch, *extra), 0)
        self.assertEqual(self.version(), "1.0.0")
        self.assertEqual(ship.sha256_file(os.path.join(self.game, ship.MANIFEST)), before)
        self.assertFalse(os.path.exists(self.game + ".staging"))
        self.assertFalse(os.path.exists(self.game + ".previous"))

    def repack(self, edit):
        out = os.path.join(self.tmp, "tampered.tar.gz")
        with tarfile.open(self.patch) as t, tarfile.open(out, "w:gz") as o:
            for m in t.getmembers():
                data = t.extractfile(m).read() if m.isfile() else None
                data = edit(m.name, data)
                if data is not None:
                    m.size = len(data)
                o.addfile(m, io.BytesIO(data) if data is not None else None)
        return out

    def test_damaged_archive_refused_by_checksum(self):
        damaged = os.path.join(self.tmp, "dist", "DEMO-linux-x64-patch.tar.gz.bad")
        data = bytearray(open(self.patch, "rb").read())
        data[len(data) // 2] ^= 0xFF
        open(damaged, "wb").write(data)
        os.replace(damaged, self.patch)
        self.assert_refused_and_v1_intact(self.patch, "--sums", self.sums)

    def test_tampered_payload_refused(self):
        bad = self.repack(lambda n, d: d + b"!" if n == "files/README.md" else d)
        self.assert_refused_and_v1_intact(bad)

    def test_tampered_manifest_refused(self):
        def edit(n, d):
            if n == ship.MANIFEST:
                m = json.loads(d)
                m["version"] = "9.9.9"
                return ship.canonical_json(m)
            return d
        self.assert_refused_and_v1_intact(self.repack(edit))

    def test_patch_for_other_base_refused(self):
        os.chmod(os.path.join(self.game, "README.md"), 0o644)
        self.assertEqual(run("apply", "--install", self.game, "--patch", self.patch), 0)
        self.assertNotEqual(run("apply", "--install", self.game, "--patch", self.patch), 0)  # base is now v2
        self.assertEqual(self.version(), "1.0.1")

    def test_damaged_install_refused(self):
        open(os.path.join(self.game, "Paks", "Game.apak"), "ab").write(b"x")
        self.assertNotEqual(run("apply", "--install", self.game, "--patch", self.patch), 0)
        self.assertFalse(os.path.exists(self.game + ".previous"))

    def test_min_engine_version_enforced_when_player_not_replaced(self):
        v4 = make_package(os.path.join(self.tmp, "v4"), "1.1.0", V2, min_engine_version="0.30.0")
        p = os.path.join(self.tmp, "p4", "patch.tar.gz")
        self.assertEqual(run("make-patch", "--from", self.v1, "--to", v4, "--out", p), 0)
        self.assert_refused_and_v1_intact(p)

    def test_rollback_without_backup_refused(self):
        self.assertNotEqual(run("rollback", "--install", self.game), 0)
        self.assertEqual(self.version(), "1.0.0")

    def test_interrupted_switch_is_recovered(self):
        self.assertEqual(run("apply", "--install", self.game, "--patch", self.patch), 0)
        os.rename(self.game, self.game + ".staging")  # as if the second rename never happened
        self.assertEqual(run("rollback", "--install", self.game), 0)
        self.assertEqual(self.version(), "1.0.0")


GUID = bytes(range(1, 17))
GUID_TEXT = "04030201-0605-0807-090A-0B0C0D0E0F10"  # GUID stored little-endian, as in RSDS and the PDB


def make_pe(rsds_guid=GUID, age=3, pdb_path="D:\\a\\build\\player\\Release\\aether_player.pdb",
            imports=("KERNEL32.dll",)) -> bytes:
    """A minimal PE32+ image: one section holding an import table and (optionally) a CodeView RSDS record."""
    va, raw = 0x1000, 0x200
    sec = bytearray(0x200)
    names_at = 20 * (len(imports) + 1)
    for i, name in enumerate(imports):
        struct.pack_into("<5I", sec, 20 * i, 0, 0, 0, va + names_at, 0)
        sec[names_at:names_at + len(name) + 1] = name.encode() + b"\0"
        names_at += len(name) + 1
    dbg, cv = 0x100, 0x120
    record = b""
    if rsds_guid is not None:
        record = b"RSDS" + rsds_guid + struct.pack("<I", age) + pdb_path.encode() + b"\0"
        sec[cv:cv + len(record)] = record
        struct.pack_into("<IIHHIIII", sec, dbg, 0, 0, 0, 0, 2, len(record), va + cv, raw + cv)
    hdr = bytearray(raw)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x40)
    hdr[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 0x58
    struct.pack_into("<H", hdr, opt, 0x20B)
    struct.pack_into("<I", hdr, opt + 108, 16)
    struct.pack_into("<II", hdr, opt + 112 + 8, va, 20 * (len(imports) + 1))
    if record:
        struct.pack_into("<II", hdr, opt + 112 + 48, va + dbg, 28)
    sh = opt + 240
    hdr[sh:sh + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", hdr, sh + 8, len(sec), va, len(sec), raw)
    return bytes(hdr) + bytes(sec)


def make_pdb(guid=GUID, info_age=1, dbi_age=3, block=512) -> bytes:
    """A minimal MSF 7.00 PDB: info stream (1) with GUID/age and a DBI stream (3) header with its age."""
    info = struct.pack("<III", 20000404, 0x12345678, info_age) + guid
    dbi = struct.pack("<iII", -1, 19990903, dbi_age) + b"\0" * 52
    directory = struct.pack("<I4I", 4, 0, len(info), 0, len(dbi)) + struct.pack("<II", 3, 4)
    blocks = [b"", b"\xff" * block, b"\xff" * block, info, dbi, directory, struct.pack("<I", 5)]
    blocks[0] = ship.PDB_MAGIC + struct.pack("<6I", block, 1, len(blocks), len(directory), 0, 6)
    return b"".join(b.ljust(block, b"\0") for b in blocks)


class PeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        open(p, "wb").write(data)
        return p

    def test_rsds_record_is_parsed(self):
        cv = ship.pe_codeview(self.write("a.exe", make_pe()))
        self.assertEqual(cv, {"guid": GUID_TEXT, "age": 3, "pdb": "D:\\a\\build\\player\\Release\\aether_player.pdb"})
        self.assertEqual(ship.symstore_key(cv["guid"], cv["age"]), "0403020106050807090A0B0C0D0E0F103")

    def test_pe_without_rsds_or_not_pe(self):
        self.assertIsNone(ship.pe_codeview(self.write("b.dll", make_pe(rsds_guid=None))))
        self.assertIsNone(ship.pe_codeview(self.write("c.txt", b"MZ" + b"\0" * 10)))
        self.assertIsNone(ship.pe_codeview(self.write("d.bin", b"\x7fELF" + b"\0" * 100)))

    def test_imports(self):
        p = self.write("e.exe", make_pe(imports=("VCRUNTIME140.dll", "KERNEL32.dll")))
        self.assertEqual(ship.pe_imports(p), ["KERNEL32.dll", "VCRUNTIME140.dll"])

    def test_pdb_guid_from_info_stream_age_from_dbi(self):
        self.assertEqual(ship.pdb_info(self.write("a.pdb", make_pdb())), {"guid": GUID_TEXT, "age": 3})
        self.assertEqual(ship.pdb_info(self.write("b.pdb", make_pdb(block=4096, dbi_age=7)))["age"], 7)
        self.assertIsNone(ship.pdb_info(self.write("c.pdb", b"not a pdb")))
        self.assertIsNone(ship.pdb_info(self.write("d.pdb", ship.PDB_MAGIC + b"\0" * 8)))


class WindowsPackageTests(unittest.TestCase):
    """package/install/apply/rollback/check-symbols for --platform windows-x64 on synthetic MSVC output."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.build = os.path.join(self.tmp, "build")
        self.player_dir = os.path.join(self.build, "player", "Release")
        os.makedirs(self.player_dir)
        open(os.path.join(self.player_dir, "aether_player.exe"), "wb").write(make_pe())
        open(os.path.join(self.player_dir, "aether_player.pdb"), "wb").write(make_pdb())
        open(os.path.join(self.player_dir, "third.dll"), "wb").write(make_pe(rsds_guid=None, imports=("USER32.dll",)))
        self.cooked = os.path.join(self.tmp, "cooked")
        os.makedirs(self.cooked)
        open(os.path.join(self.cooked, "Game.apak"), "wb").write(b"base" * 100)
        self.proj = os.path.join(self.tmp, "proj")
        os.makedirs(self.proj)
        open(os.path.join(self.proj, "README.md"), "w").write("demo")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def package(self, out, version, *extra):
        return run("package", "--build", self.build, "--cooked", self.cooked, "--version", version, "--out", out,
                   "--name", "DEMO", "--project-dir", self.proj, "--platform", "windows-x64", *extra)

    def test_zip_package_symbols_patch_and_rollback(self):
        v1, v2 = os.path.join(self.tmp, "v1"), os.path.join(self.tmp, "v2")
        self.assertEqual(self.package(v1, "1.0.0"), 0)
        again = os.path.join(self.tmp, "again")
        self.assertEqual(self.package(again, "1.0.0"), 0)
        pkg = os.path.join(v1, "DEMO-windows-x64.zip")
        self.assertEqual(ship.sha256_file(pkg), ship.sha256_file(os.path.join(again, "DEMO-windows-x64.zip")))
        with zipfile.ZipFile(pkg) as z:
            names = z.namelist()
            self.assertTrue(all(i.create_system == 0 for i in z.infolist()))
        self.assertIn("DEMO/DEMO.exe", names)
        self.assertIn("DEMO/third.dll", names)
        self.assertIn("DEMO/Paks/Game.apak", names)
        self.assertNotIn("DEMO/DEMO", names)  # no shell launcher
        self.assertFalse(any(n.startswith("DEMO/bin/") for n in names))
        syms = os.path.join(v1, "DEMO-windows-x64-symbols.zip")
        with zipfile.ZipFile(syms) as z:
            self.assertEqual(sorted(z.namelist()), ["SYMBOLS.json", "aether_player.pdb/", "aether_player.pdb/0403020106050807090A0B0C0D0E0F103/",
                                                    "aether_player.pdb/0403020106050807090A0B0C0D0E0F103/aether_player.pdb"])
            index = json.loads(z.read("SYMBOLS.json"))
        self.assertEqual([(s["binary"], s["guid"], s["age"], s["pdb"]) for s in index["symbols"]],
                         [("DEMO.exe", GUID_TEXT, 3, "aether_player.pdb")])
        game = os.path.join(self.tmp, "Games", "DEMO")
        self.assertEqual(run("install", "--archive", pkg, "--sums", os.path.join(v1, "SHA256SUMS.txt"), "--dest", game), 0)
        m = ship.verify_install(game)
        self.assertEqual((m["launcher"], m["platform"]), ("DEMO.exe", "windows-x64"))
        self.assertEqual(m["runtime_dependencies"], ["KERNEL32.dll", "USER32.dll"])
        self.assertEqual(run("check-symbols", "--install", game, "--symbols", syms), 0)

        # v2: a delta pak and a rebuilt player (new GUID); the patch is a zip.
        delta = os.path.join(self.tmp, "Game_p1.apak")
        open(delta, "wb").write(b"delta")
        open(os.path.join(self.player_dir, "aether_player.exe"), "wb").write(make_pe(rsds_guid=b"\x22" * 16, age=1))
        open(os.path.join(self.player_dir, "aether_player.pdb"), "wb").write(make_pdb(guid=b"\x22" * 16, dbi_age=1))
        self.assertEqual(self.package(v2, "1.0.1", "--extra-pak", delta), 0)
        patch = os.path.join(self.tmp, "dist", "DEMO-windows-x64-patch.zip")
        self.assertEqual(run("make-patch", "--from", pkg, "--to", os.path.join(v2, "DEMO-windows-x64.zip"), "--out", patch), 0)
        with zipfile.ZipFile(patch) as z:
            self.assertEqual(sorted(n for n in z.namelist() if n.startswith("files/") and not n.endswith("/")),
                             ["files/DEMO.exe", "files/Paks/Game_p1.apak"])
        os.makedirs(os.path.join(game, "Saved"))
        open(os.path.join(game, "Saved", "slot.sav"), "w").write("progress")
        self.assertEqual(run("apply", "--install", game, "--patch", patch, "--sums", os.path.join(self.tmp, "dist", "SHA256SUMS.txt")), 0)
        self.assertEqual(ship.verify_install(game)["version"], "1.0.1")
        self.assertEqual(run("check-symbols", "--install", game, "--symbols", os.path.join(v2, "DEMO-windows-x64-symbols.zip")), 0)
        self.assertNotEqual(run("check-symbols", "--install", game, "--symbols", syms), 0)  # v1 symbols for v2
        self.assertEqual(run("rollback", "--install", game), 0)
        self.assertEqual(ship.verify_install(game)["version"], "1.0.0")
        self.assertEqual(open(os.path.join(game, "Saved", "slot.sav")).read(), "progress")

        # A damaged zip patch is refused by its checksum and without one; v1 stays.
        bad = bytearray(open(patch, "rb").read())
        bad[len(bad) // 2] ^= 0xFF
        open(patch, "wb").write(bad)
        os.rename(game + ".previous", os.path.join(self.tmp, "prev-aside"))
        self.assertNotEqual(run("apply", "--install", game, "--patch", patch, "--sums", os.path.join(self.tmp, "dist", "SHA256SUMS.txt")), 0)
        self.assertNotEqual(run("apply", "--install", game, "--patch", patch), 0)
        self.assertEqual(ship.verify_install(game)["version"], "1.0.0")
        self.assertFalse(os.path.exists(game + ".staging"))

    def test_mismatched_pdb_refused_at_package_time(self):
        open(os.path.join(self.player_dir, "aether_player.pdb"), "wb").write(make_pdb(dbi_age=9))
        self.assertNotEqual(self.package(os.path.join(self.tmp, "out"), "1.0.0"), 0)

    def test_missing_pdb_refused_unless_no_symbols(self):
        os.remove(os.path.join(self.player_dir, "aether_player.pdb"))
        self.assertNotEqual(self.package(os.path.join(self.tmp, "out"), "1.0.0"), 0)
        self.assertEqual(self.package(os.path.join(self.tmp, "out2"), "1.0.0", "--no-symbols"), 0)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "out2", "DEMO-windows-x64-symbols.zip")))

    def test_zip_with_traversal_or_symlink_refused(self):
        for name, attr in (("../evil", 0), ("C:/evil", 0), ("x/link", (stat.S_IFLNK | 0o777) << 16)):
            bad = os.path.join(self.tmp, "bad.zip")
            with zipfile.ZipFile(bad, "w") as z:
                info = zipfile.ZipInfo(name)
                info.create_system, info.external_attr = 3, attr
                z.writestr(info, b"x")
            with self.assertRaises(ship.ShipError):
                ship.safe_extract(bad, os.path.join(self.tmp, "x"))


class SwitchTests(unittest.TestCase):
    """Folder switching without links, and files-in-use failures (as Windows reports them) on any OS."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.v1 = make_package(self.tmp, "1.0.0", V1)
        self.v2 = make_package(self.tmp, "1.0.1", V2)
        self.patch = os.path.join(self.tmp, "dist", "DEMO-linux-x64-patch.zip")
        self.assertEqual(run("make-patch", "--from", self.v1, "--to", self.v2, "--out", self.patch), 0)
        self.game = os.path.join(self.tmp, "Program Files", "DEMO")  # a path with a space
        self.assertEqual(run("install", "--archive", self.v1, "--dest", self.game), 0)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_zip_patch_on_tar_install_and_no_links(self):
        self.assertTrue(zipfile.is_zipfile(self.patch))
        self.assertEqual(run("apply", "--install", self.game + os.sep, "--patch", self.patch), 0)
        for root in (self.game, self.game + ".previous"):
            for dirpath, dirs, names in os.walk(root):
                for n in dirs + names:
                    self.assertFalse(os.path.islink(os.path.join(dirpath, n)))
        self.assertEqual(ship.verify_install(self.game)["version"], "1.0.1")

    def test_install_in_use_fails_clearly_and_is_left_intact(self):
        real = os.replace
        game = self.game

        def locked(src, dst):
            if os.path.abspath(src) == game:
                raise PermissionError(32, "The process cannot access the file because it is being used by another process", src)
            return real(src, dst)
        err = io.StringIO()
        with unittest.mock.patch("os.replace", locked), unittest.mock.patch("sys.stderr", err):
            self.assertNotEqual(run("apply", "--install", game, "--patch", self.patch), 0)
        self.assertIn("in use", err.getvalue())
        self.assertEqual(ship.verify_install(game)["version"], "1.0.0")
        self.assertFalse(os.path.exists(game + ".staging"))
        self.assertFalse(os.path.exists(game + ".previous"))

    def test_second_rename_failure_is_recovered(self):
        real = os.replace
        game = self.game

        def fail_second(src, dst):
            if src.endswith(".staging") and dst == game:
                raise PermissionError(5, "Access is denied", dst)
            return real(src, dst)
        with unittest.mock.patch("os.replace", fail_second), unittest.mock.patch("sys.stderr", io.StringIO()):
            self.assertNotEqual(run("apply", "--install", game, "--patch", self.patch), 0)
        self.assertFalse(os.path.exists(game))
        self.assertEqual(run("verify", "--install", game + ".staging"), 0)
        self.assertEqual(run("rollback", "--install", game), 0)  # recovers v2 first, then rolls back
        self.assertEqual(ship.verify_install(game)["version"], "1.0.0")


# ------------------------------------------------------------------ macOS

U_PLAYER = bytes(range(16))
U_VULKAN = bytes(range(16, 32))
U_FOO = bytes(range(32, 48))
U_MVK = bytes(range(48, 64))


def _lc_str(cmd: int, head: bytes, text: str) -> bytes:
    """A load command whose string follows <head> (the fields after cmd/cmdsize/offset), padded to 8 bytes."""
    raw = text.encode() + b"\0"
    size = (12 + len(head) + len(raw) + 7) & ~7
    return struct.pack("<III", cmd, size, 12 + len(head)) + head + raw + b"\0" * (size - 12 - len(head) - len(raw))


def make_macho(uid: bytes, dylibs=(), rpaths=(), id_name=None, filetype=2, payload=b"code") -> bytes:
    """A 64-bit little-endian (arm64) Mach-O with LC_UUID, LC_ID_DYLIB, LC_LOAD_DYLIB and LC_RPATH commands."""
    cmds = [struct.pack("<II", ship.LC_UUID, 24) + uid]
    dylib_head = struct.pack("<III", 2, 0x10000, 0x10000)  # timestamp, current, compatibility version
    if id_name:
        cmds.append(_lc_str(ship.LC_ID_DYLIB, dylib_head, id_name))
    cmds += [_lc_str(ship.LC_LOAD_DYLIB, dylib_head, d) for d in dylibs]
    cmds += [_lc_str(ship.LC_RPATH, b"", r) for r in rpaths]
    body = b"".join(cmds)
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, filetype, len(cmds), len(body), 0, 0)
    return header + body + payload


def make_fat(*slices: bytes) -> bytes:
    out = bytearray(struct.pack(">II", 0xCAFEBABE, len(slices)))
    off = 4096
    blobs = []
    for sl in slices:
        out += struct.pack(">iiIII", 0x0100000C, 0, off, len(sl), 12)
        blobs.append((off, sl))
        off += (len(sl) + 4095) & ~4095
    for o, sl in blobs:
        out += b"\0" * (o - len(out)) + sl
    return bytes(out)


class FakeXcode:
    """Stands in for dsymutil/strip/install_name_tool/codesign: records calls, makes a dSYM that copies the
    binary (same UUID, or <dsym_uuid>), and rewrites install names in place like install_name_tool."""

    def __init__(self, dsym_uuid=None):
        self.calls = []
        self.dsym_uuid = dsym_uuid

    def __call__(self, *args):
        self.calls.append(args)
        tool = args[0]
        if tool == "dsymutil":
            exe, dsym = args[1], args[3]
            dwarf = os.path.join(dsym, "Contents", "Resources", "DWARF", os.path.basename(exe))
            os.makedirs(os.path.dirname(dwarf))
            data = make_macho(self.dsym_uuid, filetype=10) if self.dsym_uuid else open(exe, "rb").read() + b"DWARF"
            open(dwarf, "wb").write(data)
            open(os.path.join(dsym, "Contents", "Info.plist"), "w").write("<plist/>")
        elif tool == "install_name_tool" and args[1] in ("-change", "-id"):
            path = args[-1]
            data = open(path, "rb").read()
            if args[1] == "-change":
                old, new = args[2], args[3]
            else:
                old, new = ship.macho_info(path)["id"], args[2]
            assert len(new) <= len(old), "fake install_name_tool only shortens names"
            data = data.replace(old.encode() + b"\0", new.encode().ljust(len(old) + 1, b"\0"))
            open(path, "wb").write(data)


class MachOTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        open(p, "wb").write(data)
        return p

    def test_load_commands_are_parsed(self):
        p = self.write("x", make_macho(U_PLAYER, dylibs=["/usr/lib/libSystem.B.dylib", "@rpath/libvulkan.1.dylib"],
                                       rpaths=["@executable_path/../Frameworks", "/opt/homebrew/lib"]))
        info = ship.macho_info(p)
        self.assertEqual(info["uuids"], ["00010203-0405-0607-0809-0A0B0C0D0E0F"])
        self.assertEqual(info["dylibs"], ["/usr/lib/libSystem.B.dylib", "@rpath/libvulkan.1.dylib"])
        self.assertEqual(info["rpaths"], ["@executable_path/../Frameworks", "/opt/homebrew/lib"])
        self.assertIsNone(info["id"])
        lib = self.write("l", make_macho(U_VULKAN, id_name="@rpath/libvulkan.1.dylib", filetype=6))
        self.assertEqual(ship.macho_info(lib)["id"], "@rpath/libvulkan.1.dylib")

    def test_fat_binary_lists_every_slice_uuid(self):
        p = self.write("fat", make_fat(make_macho(U_PLAYER), make_macho(U_FOO)))
        self.assertEqual(len(ship.macho_uuids(p)), 2)
        self.assertEqual(ship.macho_uuids(p)[1], str(__import__("uuid").UUID(bytes=U_FOO)).upper())

    def test_not_macho_or_truncated(self):
        self.assertIsNone(ship.macho_info(self.write("elf", b"\x7fELF" + b"\0" * 60)))
        self.assertIsNone(ship.macho_info(self.write("java", struct.pack(">II", 0xCAFEBABE, 52) + b"\0" * 40)))
        truncated = make_macho(U_PLAYER, dylibs=["/usr/lib/libSystem.B.dylib"])[:40]
        self.assertEqual(ship.macho_info(self.write("t", truncated))["uuids"], [])

    def test_resolve_dylib(self):
        lib_dir = os.path.join(self.tmp, "lib")
        os.makedirs(lib_dir)
        lib = self.write("lib/libfoo.dylib", b"x")
        exe = os.path.join(self.tmp, "bin", "exe")
        self.assertEqual(ship.resolve_dylib("@rpath/libfoo.dylib", exe, exe, ["/nope", lib_dir]), os.path.realpath(lib))
        self.assertEqual(ship.resolve_dylib("@executable_path/../lib/libfoo.dylib", exe, exe, []), os.path.realpath(lib))
        self.assertIsNone(ship.resolve_dylib("@rpath/libbar.dylib", exe, exe, [lib_dir]))
        self.assertTrue(ship.is_system_dylib("/usr/lib/libc++.1.dylib"))
        self.assertTrue(ship.is_system_dylib("/System/Library/Frameworks/Metal.framework/Versions/A/Metal"))
        self.assertFalse(ship.is_system_dylib("/opt/homebrew/lib/libvulkan.1.dylib"))


class MacPackageTests(unittest.TestCase):
    """package/install/apply/rollback/check-symbols for --platform macos-arm64 on synthetic Mach-O build output."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.build = os.path.join(self.tmp, "build")
        brew = os.path.join(self.tmp, "brew", "lib")
        os.makedirs(os.path.join(self.build, "player"))
        os.makedirs(brew)
        self.foo_path = os.path.join(brew, "libfoo.1.dylib")
        open(os.path.join(self.build, "player", "aether_player"), "wb").write(make_macho(
            U_PLAYER, dylibs=["/usr/lib/libSystem.B.dylib", "@rpath/libvulkan.1.dylib", self.foo_path],
            rpaths=[brew]))
        open(os.path.join(brew, "libvulkan.1.4.dylib"), "wb").write(make_macho(
            U_VULKAN, id_name=os.path.join(brew, "libvulkan.1.dylib"), filetype=6,
            dylibs=["/usr/lib/libSystem.B.dylib", self.foo_path], rpaths=[brew]))
        os.symlink("libvulkan.1.4.dylib", os.path.join(brew, "libvulkan.1.dylib"))
        open(self.foo_path, "wb").write(make_macho(U_FOO, id_name=self.foo_path, filetype=6,
                                                   dylibs=["/usr/lib/libc++.1.dylib"]))
        open(os.path.join(brew, "libMoltenVK.dylib"), "wb").write(make_macho(
            U_MVK, id_name=os.path.join(brew, "libMoltenVK.dylib"), filetype=6,
            dylibs=["/System/Library/Frameworks/Metal.framework/Versions/A/Metal"]))
        icd_dir = os.path.join(self.tmp, "brew", "etc", "vulkan", "icd.d")
        os.makedirs(icd_dir)
        self.icd = os.path.join(icd_dir, "MoltenVK_icd.json")
        json.dump({"file_format_version": "1.0.0",
                   "ICD": {"library_path": "../../../lib/libMoltenVK.dylib", "api_version": "1.2.0"}}, open(self.icd, "w"))
        self.cooked = os.path.join(self.tmp, "cooked")
        os.makedirs(self.cooked)
        open(os.path.join(self.cooked, "Game.apak"), "wb").write(b"base" * 100)
        self.proj = os.path.join(self.tmp, "proj")
        os.makedirs(self.proj)
        open(os.path.join(self.proj, "README.md"), "w").write("demo")
        self.xcode = FakeXcode()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def package(self, out, version, *extra, xcode=None):
        with unittest.mock.patch.object(ship, "macos_tool", xcode or self.xcode), \
                unittest.mock.patch.dict(os.environ, {"VK_ICD_FILENAMES": self.icd, "VK_DRIVER_FILES": ""}):
            return run("package", "--build", self.build, "--cooked", self.cooked, "--version", version, "--out", out,
                       "--name", "DEMO", "--project-dir", self.proj, "--platform", "macos-arm64", *extra)

    def test_bundle_package_symbols_patch_and_rollback(self):
        v1, v2 = os.path.join(self.tmp, "v1"), os.path.join(self.tmp, "v2")
        self.assertEqual(self.package(v1, "1.2.3"), 0)
        pkg = os.path.join(v1, "DEMO-macos-arm64.tar.gz")
        syms = os.path.join(v1, "DEMO-macos-arm64-symbols.tar.gz")
        exe = "DEMO.app/Contents/MacOS/DEMO"
        with tarfile.open(pkg) as t:
            members = {m.name: m for m in t.getmembers()}
            plist = t.extractfile("DEMO/DEMO.app/Contents/Info.plist").read().decode()
            icd = json.loads(t.extractfile("DEMO/DEMO.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json").read())
        for rel in (exe, "DEMO.app/Contents/Frameworks/libvulkan.1.dylib", "DEMO.app/Contents/Frameworks/libfoo.1.dylib",
                    "DEMO.app/Contents/Frameworks/libMoltenVK.dylib", "DEMO.app/Contents/Resources/Paks/Game.apak",
                    "GAME_MANIFEST.json", "README.md"):
            self.assertIn("DEMO/" + rel, members)
        self.assertEqual(members["DEMO/" + exe].mode, 0o755)
        self.assertEqual(members["DEMO/DEMO.app/Contents/Resources/Paks/Game.apak"].mode, 0o644)
        self.assertNotIn("DEMO/Paks", members)
        self.assertNotIn("DEMO/bin/aether_player", members)
        self.assertIn("<key>CFBundleExecutable</key>\n\t<string>DEMO</string>", plist)
        self.assertIn("<string>1.2.3</string>", plist)
        self.assertEqual(icd["ICD"]["library_path"], "../../../Frameworks/libMoltenVK.dylib")
        # Install names rewritten to @rpath, build rpath replaced by the bundle's Frameworks folder, all re-signed.
        calls = self.xcode.calls
        self.assertIn(("install_name_tool", "-add_rpath", "@executable_path/../Frameworks"), [c[:3] for c in calls])
        self.assertTrue(any(c[:2] == ("install_name_tool", "-delete_rpath") and c[-1].endswith("/MacOS/DEMO") for c in calls))
        self.assertEqual(sum(1 for c in calls if c[0] == "codesign"), 4)
        self.assertLess(next(i for i, c in enumerate(calls) if c[0] == "dsymutil"),
                        next(i for i, c in enumerate(calls) if c[0] == "strip"))

        dest = os.path.join(self.tmp, "Games", "DEMO")
        self.assertEqual(run("install", "--archive", pkg, "--sums", os.path.join(v1, "SHA256SUMS.txt"), "--dest", dest), 0)
        m = ship.verify_install(dest)
        self.assertEqual(m["launcher"], exe)
        self.assertEqual(ship.player_path(m), exe)
        self.assertEqual(m["paks"], ["DEMO.app/Contents/Resources/Paks/Game.apak"])
        self.assertEqual(m["runtime_dependencies"], ["/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
                                                     "/usr/lib/libSystem.B.dylib", "/usr/lib/libc++.1.dylib"])
        for b in m["binaries"]:
            for dep in ship.macho_info(os.path.join(dest, *b["path"].split("/")))["dylibs"]:
                self.assertTrue(ship.is_system_dylib(dep) or dep.startswith("@rpath/"), dep)
        self.assertEqual(ship.macho_info(os.path.join(dest, "DEMO.app/Contents/Frameworks/libfoo.1.dylib"))["id"],
                         "@rpath/libfoo.1.dylib")
        self.assertEqual(os.stat(os.path.join(dest, *exe.split("/"))).st_mode & 0o777, 0o755)
        player_uuid = str(__import__("uuid").UUID(bytes=U_PLAYER)).upper()
        self.assertEqual(m["symbols"][0]["uuids"], [player_uuid])
        self.assertEqual(m["symbols"][0]["debug_file"], f"{player_uuid}/DEMO.dSYM/Contents/Resources/DWARF/DEMO")
        self.assertEqual(run("check-symbols", "--install", dest, "--symbols", syms), 0)

        # Deterministic, and v2 differs only by its delta pak (Info.plist carries the numeric version only).
        again = os.path.join(self.tmp, "again")
        self.assertEqual(self.package(again, "1.2.3", xcode=FakeXcode()), 0)
        self.assertEqual(ship.sha256_file(pkg), ship.sha256_file(os.path.join(again, "DEMO-macos-arm64.tar.gz")))
        delta = os.path.join(self.tmp, "Game_p1.apak")
        open(delta, "wb").write(b"delta")
        self.assertEqual(self.package(v2, "1.2.3+p1", "--extra-pak", delta, xcode=FakeXcode()), 0)
        patch = os.path.join(self.tmp, "patch.tar.gz")
        self.assertEqual(run("make-patch", "--from", pkg, "--to", os.path.join(v2, "DEMO-macos-arm64.tar.gz"), "--out", patch), 0)
        with tarfile.open(patch) as t:
            names = sorted(n.name for n in t.getmembers() if n.isfile() and n.name.startswith("files/"))
        self.assertEqual(names, ["files/DEMO.app/Contents/Resources/Paks/Game_p1.apak"])
        os.makedirs(os.path.join(dest, "Saved"))
        open(os.path.join(dest, "Saved", "a.sav"), "w").write("save")
        self.assertEqual(run("apply", "--install", dest, "--patch", patch), 0)
        self.assertTrue(os.path.isfile(os.path.join(dest, "DEMO.app/Contents/Resources/Paks/Game_p1.apak")))
        self.assertEqual(ship.verify_install(dest)["paks"], ["DEMO.app/Contents/Resources/Paks/Game.apak",
                                                            "DEMO.app/Contents/Resources/Paks/Game_p1.apak"])
        self.assertEqual(run("rollback", "--install", dest), 0)
        self.assertFalse(os.path.exists(os.path.join(dest, "DEMO.app/Contents/Resources/Paks/Game_p1.apak")))
        self.assertTrue(os.path.isfile(os.path.join(dest, "Saved", "a.sav")))
        self.assertEqual(os.stat(os.path.join(dest, *exe.split("/"))).st_mode & 0o777, 0o755)

    def test_check_symbols_refuses_a_dsym_with_another_uuid(self):
        v1 = os.path.join(self.tmp, "v1")
        self.assertEqual(self.package(v1, "1.0.0"), 0)
        dest = os.path.join(self.tmp, "inst")
        self.assertEqual(run("install", "--archive", os.path.join(v1, "DEMO-macos-arm64.tar.gz"), "--dest", dest), 0)
        syms = os.path.join(v1, "DEMO-macos-arm64-symbols.tar.gz")
        bad = os.path.join(self.tmp, "bad-symbols.tar.gz")
        with tarfile.open(syms) as t, tarfile.open(bad, "w:gz") as o:
            for mem in t.getmembers():
                data = t.extractfile(mem).read() if mem.isfile() else None
                if mem.name.endswith("/DWARF/DEMO"):
                    data = make_macho(U_FOO, filetype=10)
                    mem.size = len(data)
                o.addfile(mem, io.BytesIO(data) if data is not None else None)
        self.assertEqual(run("check-symbols", "--install", dest, "--symbols", bad), 1)
        self.assertEqual(run("check-symbols", "--install", dest, "--symbols", syms), 0)

    def test_check_symbols_refuses_a_swapped_dylib(self):
        v1 = os.path.join(self.tmp, "v1")
        self.assertEqual(self.package(v1, "1.0.0"), 0)
        dest = os.path.join(self.tmp, "inst")
        self.assertEqual(run("install", "--archive", os.path.join(v1, "DEMO-macos-arm64.tar.gz"), "--dest", dest), 0)
        m = ship.load_installed(dest)
        m["binaries"][1]["uuids"] = ["00000000-0000-0000-0000-000000000000"]  # manifest/binary disagree
        with unittest.mock.patch.object(ship, "verify_install", return_value=m):
            self.assertEqual(run("check-symbols", "--install", dest, "--symbols",
                                 os.path.join(v1, "DEMO-macos-arm64-symbols.tar.gz")), 1)

    def test_mismatched_dsym_refused_at_package_time(self):
        self.assertEqual(self.package(os.path.join(self.tmp, "v1"), "1.0.0", xcode=FakeXcode(dsym_uuid=U_FOO)), 1)

    def test_missing_dependency_refused(self):
        os.remove(self.foo_path)
        self.assertEqual(self.package(os.path.join(self.tmp, "v1"), "1.0.0"), 1)

    def test_player_without_uuid_refused(self):
        data = make_macho(U_PLAYER)
        data = data.replace(struct.pack("<II", ship.LC_UUID, 24), struct.pack("<II", 0x7FFFFFFF, 24))
        open(os.path.join(self.build, "player", "aether_player"), "wb").write(data)
        self.assertEqual(self.package(os.path.join(self.tmp, "v1"), "1.0.0"), 1)


@unittest.skipUnless(shutil.which("cc") and shutil.which("objcopy"), "needs cc and objcopy")
class SymbolTests(unittest.TestCase):
    def test_split_debug_keeps_build_id(self):
        tmp = tempfile.mkdtemp()
        try:
            src = os.path.join(tmp, "m.c")
            open(src, "w").write("int main(void){return 0;}\n")
            exe = os.path.join(tmp, "m")
            subprocess.run(["cc", "-g", "-Wl,--build-id", src, "-o", exe], check=True)
            bid = ship.elf_build_id(exe)
            self.assertRegex(bid or "", r"^[0-9a-f]{40}$")
            dbg = exe + ".debug"
            subprocess.run(["objcopy", "--only-keep-debug", exe, dbg], check=True)
            subprocess.run(["objcopy", "--strip-debug", f"--add-gnu-debuglink={dbg}", exe], check=True)
            self.assertEqual(ship.elf_build_id(exe), bid)
            self.assertEqual(ship.elf_build_id(dbg), bid)
            self.assertIsNone(ship.elf_build_id(src))
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
