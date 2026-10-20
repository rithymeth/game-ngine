#!/usr/bin/env python3
"""Checks API stability annotations (Phase 37 step 3, docs/design/UPGRADE_PLAN.md).

Every top-level module with public headers (<module>/include/aether/...) has a level in
docs/design/api_stability.txt, every level is stable/experimental/internal, and every header-level
`// @stability: <level>` override names a valid level. With --table it prints each module's headers
by level.

Stable headers are also frozen (Phase 48 step 3): docs/design/api_frozen.txt records the public names each one
declares (types, functions, macros, aliases), and a name that disappears is an error unless it is listed in
docs/design/api_removed.txt ("<header> <name>", added after a deprecation release). New names are fine.
docs/design/api_frozen_signatures.txt records a digest of normalized function-like declarations. A signature
change is an error until the baseline is reviewed and updated. --update-frozen adds new names and refreshes
the reviewed signature digests.
"""
import argparse
import hashlib
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
SIGNATURE_NAME = re.compile(r"\b(?:operator\s*[^\w\s(]+|[A-Za-z_]\w*)\s*\(")


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


def public_signatures(text):
    """Collect normalized function-like declarations, excluding inline function bodies."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = strip_bodies(text)
    signatures = set()
    for fragment in re.split(r"[;{}]", text):
        fragment = " ".join(fragment.split())
        if not fragment or not SIGNATURE_NAME.search(fragment):
            continue
        # Whitespace between identifiers matters; whitespace around punctuation does not.
        fragment = re.sub(r"\s*([,()<>*&\[\]=:])\s*", r"\1", fragment)
        signatures.add(fragment)
    return sorted(signatures)


def signature_digest(text):
    declarations = "\n".join(public_signatures(text))
    return hashlib.sha256(declarations.encode("utf-8")).hexdigest()


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


def read_signatures(path):
    signatures = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    header, _, digest = line.partition(":")
                    signatures[header.strip()] = digest.strip()
    return signatures


INCLUDE = re.compile(r'^\s*#\s*include\s+"(aether/[^"]+)"', re.M)


def header_level(path, module_levels):
    """The level of one header: its `@stability` tag, else its module's."""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            t = TAG.match(line)
            if t and t.group(1) in LEVELS:
                return t.group(1)
    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
    return module_levels.get(rel.split("/")[0], "experimental")


def find_header(name):
    """The file an `#include "aether/..."` names, in any module's include folder."""
    for m in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, m, "include", name)
        if os.path.isfile(p):
            return p
    return None


def closure(path, seen):
    for name in INCLUDE.findall(open(path, encoding="utf-8", errors="replace").read()):
        if name in seen:
            continue
        seen.add(name)
        dep = find_header(name)
        if dep:
            closure(dep, seen)
    return seen


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
    ap.add_argument("--signatures", default=os.path.join(ROOT, "docs", "design", "api_frozen_signatures.txt"))
    ap.add_argument("--removed", default=os.path.join(ROOT, "docs", "design", "api_removed.txt"))
    ap.add_argument("--consumer", action="append", default=[], metavar="DIR",
                    help="check that the sources in DIR include only stable headers (and what those include is stable or internal)")
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
    stable_signatures = {}
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
                        text = f.read()
                    header = os.path.relpath(path, ROOT).replace(os.sep, "/")
                    stable_headers[header] = set(public_names(text))
                    stable_signatures[header] = signature_digest(text)
        table[m] = counts
    # A stable header may include only stable or internal ones: anything else would put an unfrozen layout in the frozen API.
    for header in sorted(stable_headers):
        for name in sorted(closure(os.path.join(ROOT, header), set())):
            dep = find_header(name)
            if dep and header_level(dep, levels) == "experimental":
                problems.append(f"{header} includes {name}, which is experimental (make it stable or internal)")
    # A consumer (a game built on the frozen API) may name only stable headers.
    for folder in args.consumer:
        for base, _, files in os.walk(os.path.join(ROOT, folder) if not os.path.isabs(folder) else folder):
            for fn in sorted(files):
                if not fn.endswith((".h", ".hpp", ".cpp")):
                    continue
                path = os.path.join(base, fn)
                for name in INCLUDE.findall(open(path, encoding="utf-8", errors="replace").read()):
                    dep = find_header(name)
                    if dep is None:
                        problems.append(f"{os.path.relpath(path, ROOT)}: includes {name}, which does not exist")
                    elif header_level(dep, levels) != "stable":
                        problems.append(f"{os.path.relpath(path, ROOT)}: includes {name}, which is {header_level(dep, levels)}, not stable")
    frozen = read_frozen(args.frozen)
    removed = read_removed(args.removed)
    signatures = read_signatures(args.signatures)
    if args.update_frozen:
        for header, names in stable_headers.items():
            frozen[header] = frozen.get(header, set()) | names
        with open(args.frozen, "w", encoding="utf-8") as f:
            f.write("# Public names of stable headers (Phase 48 step 3). A name may not disappear unless it is in api_removed.txt.\n")
            f.write("# Written by: tools/check_api_stability.py --update-frozen\n")
            for header in sorted(frozen):
                if header in stable_headers:
                    f.write(header + ": " + " ".join(sorted(frozen[header])) + "\n")
        signatures.update(stable_signatures)
        with open(args.signatures, "w", encoding="utf-8") as f:
            f.write("# SHA-256 of normalized function-like declarations in stable headers (Phase 48 step 3).\n")
            f.write("# Written by: tools/check_api_stability.py --update-frozen\n")
            for header in sorted(signatures):
                if header in stable_signatures:
                    f.write(header + ": " + signatures[header] + "\n")
        print(f"check_api_stability: wrote {os.path.relpath(args.frozen, ROOT)} and {os.path.relpath(args.signatures, ROOT)} ({len(stable_headers)} headers)")
        return 0
    for header, names in sorted(stable_headers.items()):
        if header not in frozen:
            problems.append(f"{header} is stable but has no baseline in {os.path.relpath(args.frozen, ROOT)} (run --update-frozen)")
            continue
        for name in sorted(frozen[header] - names):
            if (header, name) not in removed:
                problems.append(f"{header}: '{name}' was removed from a stable header (deprecate it for a release first, then list it in {os.path.relpath(args.removed, ROOT)})")
        if header not in signatures:
            problems.append(f"{header} has no signature baseline in {os.path.relpath(args.signatures, ROOT)} (run --update-frozen)")
        elif signatures[header] != stable_signatures[header]:
            problems.append(f"{header}: public declaration signatures changed (review and update {os.path.relpath(args.signatures, ROOT)})")
    for header in sorted(set(frozen) - set(stable_headers)):
        problems.append(f"{header} is in the frozen baseline but is no longer a stable header")
    for header in sorted(set(signatures) - set(stable_signatures)):
        problems.append(f"{header} has a signature baseline but is no longer a stable header")
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
