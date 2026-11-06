"""Every source file in a module is compiled by it.

A .cpp under `<module>/src` that the module's CMakeLists never lists is not a compile error and not a test failure --
it is simply never built, so it rots silently and its tests never run. That is exactly what happened to
`bench/bench_nav.cpp`: the benchmark existed for months and had never been compiled, and running it for the first time
reproduced the conclusion it was written about.

The check is deliberately textual (does the filename appear in the CMakeLists), because that is how the files are listed.
"""

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent),
                        help="repository root (default: this script's parent's parent)")
    args = parser.parse_args()
    root = Path(args.root)

    missing: list[str] = []
    checked = 0
    for module in sorted(p for p in root.iterdir() if p.is_dir()):
        src = module / "src"
        if not src.is_dir():
            continue
        # A module's sources may be listed by a CMakeLists anywhere under it: the editor keeps its list in
        # editor/src/ui/CMakeLists.txt, not at the module root.
        cmakes = sorted(module.rglob("CMakeLists.txt"))
        if not cmakes:
            continue
        text = "".join(path.read_text(encoding="utf-8", errors="replace") for path in cmakes)
        for source in sorted(src.rglob("*.cpp")):
            checked += 1
            if source.name not in text:
                missing.append(f"{module.name}/{source.relative_to(module)}")

    # The same bug one directory over: a test file missing from tests/CMakeLists.txt never runs, so its tests
    # silently do not exist. Checked the same textual way.
    tests_dir = root / "tests"
    tests_cmake = tests_dir / "CMakeLists.txt"
    if tests_cmake.exists():
        text = tests_cmake.read_text(encoding="utf-8", errors="replace")
        for source in sorted(tests_dir.glob("test_*.cpp")):
            checked += 1
            if source.name not in text:
                missing.append(f"tests/{source.name}")

    for path in missing:
        print(f"{path}: present but not listed in its module's CMakeLists")
    print(f"{checked} source files checked, {len(missing)} not compiled")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())