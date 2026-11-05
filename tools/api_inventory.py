#!/usr/bin/env python3
"""Core API inventory (M8.1, docs/design/M8_PRODUCTION_PLAN.md).

Writes or checks two generated, sorted files:

  docs/design/api_inventory_cpp.txt   every installed Aether header (<module>/include/**) with its stability
                                      level, the types it declares and its normalized function-like
                                      declarations (the same normalization as the Phase 48.3 freeze checker,
                                      tools/check_api_stability.py). Installed editor/MCP headers are listed by
                                      path only (internal tooling, not game API).
  docs/design/api_inventory_luau.txt  every global table, function and handle method a game script sees. It is
                                      produced by the test ApiInventory_LuauMatchesCheckedInFile in aether_tests,
                                      which also fails on an unreviewed difference.

  python3 tools/api_inventory.py                    check the C++ inventory (CI; exits 1 on a difference)
  python3 tools/api_inventory.py --tests BIN        also check the Luau inventory with that test binary
  python3 tools/api_inventory.py --update [--tests BIN]   regenerate after an intended API change
"""
import argparse
import difflib
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_api_stability as stability  # noqa: E402

ROOT = stability.ROOT
CPP = os.path.join(ROOT, "docs", "design", "api_inventory_cpp.txt")
LUAU = os.path.join(ROOT, "docs", "design", "api_inventory_luau.txt")
# Installed by aether_install_target but editor/tool internals (not inventoried declaration by declaration).
PATH_ONLY_DIRS = ["editor/src", "mcp"]
TYPE_DECL = re.compile(r"\b(class|struct|union|enum\s+class|enum\s+struct|enum)\s+(?:alignas\([^)]*\)\s*)?([A-Za-z_]\w*)\s*(?:final\s*)?[:{]")
HEADER_EXT = (".h", ".hpp", ".inl")


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def headers_under(folder):
    out = []
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        out.extend(os.path.join(base, f) for f in files if f.endswith(HEADER_EXT))
    return sorted(out, key=rel)


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def cpp_inventory():
    levels = stability.read_levels(os.path.join(ROOT, "docs", "design", "api_stability.txt"))
    lines = ["# C++ API inventory of installed Aether headers (M8.1). Generated; do not edit by hand.",
             "# Regenerate: python3 tools/api_inventory.py --update   (review the diff: it is the API change)",
             "# [header] level, then 'type <kind> <name>' and normalized declarations, sorted."]
    count = 0
    for module in stability.modules_with_headers():
        for path in headers_under(os.path.join(ROOT, module, "include")):
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            level = stability.header_level(path, levels)
            lines.append(f"[{rel(path)}] {level}")
            body = stability.strip_bodies(strip(text))
            types = sorted({f"type {' '.join(k.split())} {n}" for k, n in TYPE_DECL.findall(body)})
            lines.extend("  " + t for t in types)
            lines.extend("  " + s for s in stability.public_signatures(text))
            count += 1
    for folder in PATH_ONLY_DIRS:
        for path in headers_under(os.path.join(ROOT, folder)):
            lines.append(f"[{rel(path)}] internal-tooling")
            count += 1
    return "\n".join(lines) + "\n", count


def run_luau(binary, update):
    env = dict(os.environ, AETHER_TEST_FILTER="ApiInventory_Luau")
    if update:
        env["AETHER_UPDATE_API_INVENTORY"] = "1"
    proc = subprocess.run([binary], env=env, capture_output=True, text=True)
    out = "\n".join(l for l in proc.stdout.splitlines() if l.startswith(("  ", "[PASS]", "[FAIL]")))
    print(out)
    if "  skipped:" in proc.stdout:
        print("api_inventory: this test binary lacks optional modules the Luau inventory records; use a full build")
        return 1
    if "[PASS] ApiInventory_LuauMatchesCheckedInFile" not in proc.stdout:
        print("api_inventory: the Luau inventory test did not pass (or did not run: is scripting enabled?)")
        return 1
    return proc.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the checked-in inventories")
    ap.add_argument("--tests", metavar="BIN", help="aether_tests binary, for the Luau inventory")
    args = ap.parse_args()
    text, count = cpp_inventory()
    status = 0
    if args.update:
        with open(CPP, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"api_inventory: wrote {rel(CPP)} ({count} headers)")
    else:
        old = open(CPP, encoding="utf-8").read() if os.path.exists(CPP) else ""
        if old.replace("\r\n", "\n") != text:
            diff = list(difflib.unified_diff(old.splitlines(), text.splitlines(), rel(CPP), "current headers", lineterm="", n=1))
            for line in diff[:120]:
                print(line)
            if len(diff) > 120:
                print(f"... {len(diff) - 120} more diff lines")
            print("api_inventory: the public C++ API changed without updating the reviewed inventory.\n"
                  "  If the change is intended, run: python3 tools/api_inventory.py --update\n"
                  "  and commit docs/design/api_inventory_cpp.txt with the change (reviewers read that diff).\n"
                  "  Removing or changing a stable declaration needs a deprecation first: docs/manual/api_stability.md.")
            status = 1
        else:
            print(f"api_inventory: C++ inventory ok ({count} headers)")
    if args.tests:
        status |= run_luau(args.tests, args.update)
    return status


if __name__ == "__main__":
    sys.exit(main())
