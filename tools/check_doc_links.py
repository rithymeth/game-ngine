#!/usr/bin/env python3
"""Check that every relative link in the documentation points at a file that exists.

`docs/README.md` is the index and has its own gate; this checks the links *inside* the documents, which had no
check at all. The failure that prompted it: a game checklist called a design document its "canonical product
reference" for weeks while the file was never in the repository. Every row that said "the plan requires X" was
therefore unauditable, and nothing noticed.

Links are checked as written: relative to the file that contains them, with any #fragment removed. Absolute URLs,
mailto links and fragment-only links are ignored. A `games/` directory that is not present is skipped, because the
game repositories are separate checkouts -- but if it *is* present, its design documents are checked like the rest.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"(?<!&)\]\(([^)\s]+)")
SKIP_SCHEMES = ("http://", "https://", "mailto:", "#")
FENCE = re.compile(r"^\s*(```|~~~)")


def prose_lines(text: str):
    """Yield (number, line) outside fenced code blocks.

    Code samples contain things that look exactly like a link -- a C++ lambda written `[&](World& world)` was the
    first false positive this check produced -- so fenced blocks are skipped rather than parsed.
    """
    inside = False
    for number, line in enumerate(text.splitlines(), 1):
        if FENCE.match(line):
            inside = not inside
            continue
        if not inside:
            yield number, line


def documents(include_games: bool) -> list[Path]:
    found = list((ROOT / "docs").rglob("*.md"))
    # Separate checkouts: present here, absent in a clean engine-only clone. The engine's own gate leaves them
    # out so one repository's broken link cannot fail another's build; --include-games checks them on purpose.
    if include_games:
        for game_dir in sorted((ROOT / "games").glob("*/Design")):
            found.extend(game_dir.rglob("*.md"))
    return sorted(found)


def main() -> int:
    include_games = "--include-games" in sys.argv[1:]
    problems: list[str] = []
    checked = 0
    for document in documents(include_games):
        try:
            text = document.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            problems.append(f"{document}: cannot be read ({error})")
            continue
        for number, line in prose_lines(text):
            for match in LINK.finditer(line):
                target = match.group(1)
                if target.startswith(SKIP_SCHEMES):
                    continue
                path = target.split("#", 1)[0]
                if not path:
                    continue
                checked += 1
                resolved = (document.parent / path).resolve()
                if not resolved.exists():
                    problems.append(
                        f"{document.relative_to(ROOT)}:{number}: links to {target}, which does not exist"
                    )

    for problem in problems:
        print(f"check_doc_links: {problem}")
    if problems:
        print(f"check_doc_links: {len(problems)} problem(s)")
        return 1
    print(f"check_doc_links: ok ({checked} relative links across {len(documents(include_games))} documents)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
