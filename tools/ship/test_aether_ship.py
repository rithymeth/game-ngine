#!/usr/bin/env python3
"""Unit tests for tools/ship/aether_ship.py: manifests, install, patch, rollback, symbols (M8.10)."""
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

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
