#!/usr/bin/env python3
"""Checks API stability annotations (Phase 37 step 3, docs/design/UPGRADE_PLAN.md).

Every top-level module with public headers (<module>/include/aether/...) has a level in
docs/design/api_stability.txt, every level is stable/experimental/internal, and every header-level
`// @stability: <level>` override names a valid level. With --table it prints each module's headers
by level.

Stable headers are also frozen (Phase 48 step 3): docs/design/api_frozen.txt records the public names each one
declares (types, functions, macros, aliases), and a name that disappears is an error unless it is listed in
docs/design/api_removed.txt ("<header> <name>", added after a deprecation release). New names are fine.
--update-frozen adds the names of stable headers to the baseline (it never drops one).
"""
import argparse
import os
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = {"stable", "experimental", "internal"}
TAG = re.compile(r"^\s*//\s*@stability:\s*(\S+)")


KEYWORDS = set("""if for while switch return sizeof alignof decltype static_assert noexcept operator defined catch throw new delete
requires template typename class struct enum union namespace using typedef static inline constexpr const volatile explicit virtual override
final friend public private protected void int bool char float double unsigned signed long short auto else do case default break continue
typeid static_cast dynamic_cast reinterpret_cast const_cast co_await co_return co_yield consteval constinit mutable extern""".split())
DECL = re.compile(r"\b(?:class|struct|union|enum\s+class|enum\s+struct|enum|namespace|using|typedef)\s+([A-Za-z_]\w*)")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
MACRO = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)", re.M)


BODY_END = re.compile(r"(?:\)|\bconst|\bnoexcept|\boverride|\bfinal|\bmutable|->\s*[\w:<>,&* ]+)\s*$")


def strip_bodies(text):
    """Drops the inside of function bodies, so names called there are not mistaken for declarations."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{" and BODY_END.search("".join(out[-120:])):
            depth = 1
            i += 1
            while i < n and depth:
                depth += (text[i] == "{") - (text[i] == "}")
                i += 1
            out.append("{}")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def public_names(text):
    """The names a header declares: a coarse scan, enough to notice one disappearing."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    names = set(MACRO.findall(text))
    text = strip_bodies(text)
    names.update(DECL.findall(text))
    names.update(CALL.findall(text))
    return sorted(n for n in names if n not in KEYWORDS and not n.startswith("_"))


def read_frozen(path):
    frozen = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    header, _, names = line.partition(":")
                    frozen[header.strip()] = set(names.split())
    return frozen


def read_removed(path):
    removed = set()
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    removed.add(tuple(line.split(None, 1)))
    return removed


def modules_with_headers():
    out = []
    for d in sorted(os.listdir(ROOT)):
        if os.path.isdir(os.path.join(ROOT, d, "include", "aether")):
            out.append(d)
    return out


def read_levels(path):
    levels = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#")[0].strip()
            if line:
                parts = line.split()
                if len(parts) != 2:
                    raise SystemExit(f"{path}:{n}: expected '<module> <level>'")
                levels[parts[0]] = parts[1]
    return levels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", default=os.path.join(ROOT, "docs", "design", "api_stability.txt"))
    ap.add_argument("--table", action="store_true")
    ap.add_argument("--frozen", default=os.path.join(ROOT, "docs", "design", "api_frozen.txt"))
    ap.add_argument("--removed", default=os.path.join(ROOT, "docs", "design", "api_removed.txt"))
    ap.add_argument("--update-frozen", action="store_true", help="add the names of stable headers to the baseline")
    args = ap.parse_args()
    levels = read_levels(args.levels)
    problems = []
    modules = modules_with_headers()
    for m in modules:
        if m not in levels:
            problems.append(f"module '{m}' has public headers but no level in {os.path.relpath(args.levels, ROOT)}")
    for m, lv in levels.items():
        if lv not in LEVELS:
            problems.append(f"module '{m}' has level '{lv}' (use stable, experimental or internal)")
        if m not in modules:
            problems.append(f"module '{m}' is in the levels file but has no public headers")
    table = {}
    stable_headers = {}
    for m in modules:
        counts = Counter()
        for base, _, files in os.walk(os.path.join(ROOT, m, "include")):
            for fn in files:
                if not fn.endswith((".h", ".hpp")):
                    continue
                path = os.path.join(base, fn)
                level = levels.get(m, "experimental")
                with open(path, encoding="utf-8", errors="replace") as f:
                    for n, line in enumerate(f, 1):
                        t = TAG.match(line)
                        if t:
                            if t.group(1) not in LEVELS:
                                problems.append(f"{os.path.relpath(path, ROOT)}:{n}: '@stability: {t.group(1)}' (use stable, experimental or internal)")
                            else:
                                level = t.group(1)
                            break
                counts[level] += 1
                if level == "stable":
                    with open(path, encoding="utf-8", errors="replace") as f:
                        stable_headers[os.path.relpath(path, ROOT).replace(os.sep, "/")] = set(public_names(f.read()))
        table[m] = counts
    frozen = read_frozen(args.frozen)
    removed = read_removed(args.removed)
    if args.update_frozen:
        for header, names in stable_headers.items():
            frozen[header] = frozen.get(header, set()) | names
        with open(args.frozen, "w", encoding="utf-8") as f:
            f.write("# Public names of stable headers (Phase 48 step 3). A name may not disappear unless it is in api_removed.txt.\n")
            f.write("# Written by: tools/check_api_stability.py --update-frozen\n")
            for header in sorted(frozen):
                if header in stable_headers:
                    f.write(header + ": " + " ".join(sorted(frozen[header])) + "\n")
        print(f"check_api_stability: wrote {os.path.relpath(args.frozen, ROOT)} ({len(stable_headers)} headers)")
        return 0
    for header, names in sorted(stable_headers.items()):
        if header not in frozen:
            problems.append(f"{header} is stable but has no baseline in {os.path.relpath(args.frozen, ROOT)} (run --update-frozen)")
            continue
        for name in sorted(frozen[header] - names):
            if (header, name) not in removed:
                problems.append(f"{header}: '{name}' was removed from a stable header (deprecate it for a release first, then list it in {os.path.relpath(args.removed, ROOT)})")
    for header in sorted(set(frozen) - set(stable_headers)):
        problems.append(f"{header} is in the frozen baseline but is no longer a stable header")
    if args.table:
        for m in modules:
            print(f"{m:14} " + "  ".join(f"{k} {v}" for k, v in sorted(table[m].items())))
    for p in problems:
        print("check_api_stability:", p)
    if problems:
        return 1
    total = Counter()
    for c in table.values():
        total.update(c)
    print("check_api_stability: ok (" + ", ".join(f"{v} {k}" for k, v in sorted(total.items())) + " headers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
