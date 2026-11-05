#!/usr/bin/env python3
"""Check docs/design/format_versions.txt against the tree.

The table names, for every serialized format, the file that implements it. Nothing verified those
paths, so a rename could leave the table documenting a file that no longer exists -- the same class
of drift a link check catches, but in a table rather than a link.

What this checks:
  * every row parses into format / where / version / older / newer;
  * every `where` path exists;
  * the version column is a positive integer and the two migration columns are non-empty.

What it does not check: that the number matches the code. The constants are named differently per
module (kVersion, kFormatVersion, kJsonSceneVersion, LevelSequence::kVersion), and several file rows
share a module, so a guess would produce false alarms. When a format bumps, update this table by hand
and keep a fixture of the older version -- the header of the table says so.
"""

import re
import sys
from pathlib import Path

TABLE = Path(__file__).resolve().parent.parent / "docs" / "design" / "format_versions.txt"

# format may contain single spaces ("save/settings envelope", "material function"), so the path is
# matched explicitly rather than by splitting on whitespace runs.
ROW = re.compile(
    r"^(?P<format>.+?)\s+(?P<where>\S+\.(?:cpp|h|hpp))\s+(?P<version>\d+)\s+(?P<older>.+?)\s+(?P<newer>.+?)\s*$"
)


def main() -> int:
    if not TABLE.is_file():
        print(f"missing {TABLE}", file=sys.stderr)
        return 1

    root = TABLE.parent.parent.parent
    problems: list[str] = []
    rows = 0

    for number, line in enumerate(TABLE.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = ROW.match(stripped)
        if match is None:
            problems.append(f"{TABLE.name}:{number}: cannot parse row: {stripped}")
            continue
        rows += 1
        where = match.group("where")
        if not (root / where).is_file():
            problems.append(f"{TABLE.name}:{number}: {match.group('format')!r} points at {where}, which does not exist")
        if int(match.group("version")) < 1:
            problems.append(f"{TABLE.name}:{number}: version must be at least 1")

    if rows == 0:
        problems.append(f"{TABLE.name}: no rows found")

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"format versions: {rows} rows, every path exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
