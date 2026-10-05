#!/usr/bin/env python3
"""Checks API stability annotations (Phase 37 step 3, docs/design/UPGRADE_PLAN.md).

Every top-level module with public headers (<module>/include/aether/...) has a level in
docs/design/api_stability.txt, every level is stable/experimental/internal, and every header-level
`// @stability: <level>` override names a valid level. With --table it prints each module's headers
by level.
"""
import argparse
import os
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = {"stable", "experimental", "internal"}
TAG = re.compile(r"^\s*//\s*@stability:\s*(\S+)")


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
        table[m] = counts
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
