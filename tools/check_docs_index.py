#!/usr/bin/env python3
"""Check that docs/README.md is a complete index of docs/.

The root README tells readers that "all planning and design docs are listed in docs/README.md", and
that promise had drifted badly: 40 of 53 documents were not in it, including the manual chapters from
02 onwards and every readiness record (mobile, platform policy, performance, RHI lifetime). A doc that
is not in the index is one nobody finds.

Checks both directions:
  * every docs/**/*.md other than docs/README.md is linked from it;
  * every link in it points at a file that exists.
"""

import re
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"
INDEX = DOCS / "README.md"
LINK = re.compile(r"\]\(([^)\s#]+)")


def main() -> int:
    if not INDEX.is_file():
        print(f"missing {INDEX}", file=sys.stderr)
        return 1

    text = INDEX.read_text(encoding="utf-8")
    linked = {match.group(1) for match in LINK.finditer(text)}

    problems: list[str] = []

    for target in sorted(linked):
        if re.match(r"^(https?:|mailto:)", target):
            continue
        if not (DOCS / target).exists():
            problems.append(f"{INDEX.name}: links to {target}, which does not exist")

    for path in sorted(DOCS.rglob("*.md")):
        if path == INDEX:
            continue
        relative = path.relative_to(DOCS).as_posix()
        if relative not in linked:
            problems.append(f"{INDEX.name}: missing {relative}")

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"docs index: {len(linked)} links, every document listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
