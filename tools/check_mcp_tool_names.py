"""No MCP tool name is registered twice.

Two files registering the same tool name is not a compile error and not a test failure: the server keeps
both, one shadows the other, and an agent gets whichever it happens to find. That is how a duplicated
prefab tool nearly shipped -- the callers were in `mcp/prefab_tools.cpp` and the copy was in
`mcp/editor_tools.cpp`, so a grep of one file found nothing.

Exit code 1 when a name is registered more than once, so this can be a gate.
"""

import argparse
import re
import sys
from pathlib import Path

# `AddTool({"name",` or the multi-line `AddTool(\n {"name",` form.
PATTERN = re.compile(r'AddTool\(\s*\{?\s*"([a-z0-9_]+)"')


def collect(root: Path) -> dict[str, list[str]]:
    seen: dict[str, list[str]] = {}
    for path in sorted((root / "mcp").glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in PATTERN.finditer(text):
            seen.setdefault(match.group(1), []).append(path.name)
    return seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent),
                        help="repository root (default: this script's parent's parent)")
    args = parser.parse_args()
    root = Path(args.root)
    seen = collect(root)
    duplicates = {name: files for name, files in seen.items() if len(files) > 1}
    for name, files in sorted(duplicates.items()):
        print(f"{name}: registered by {', '.join(files)}")
    # Every tool has to be named in the README, which is what an MCP client author reads. The table
    # documents tools by family, so this is a name check rather than a structural one -- but a family
    # added without a row is exactly how the table stopped covering all of them before.
    readme = (root / "mcp" / "README.md")
    undocumented: list[str] = []
    if readme.exists():
        text = readme.read_text(encoding="utf-8", errors="replace")
        undocumented = sorted(name for name in seen if name not in text)
        for name in undocumented:
            print(f"{name}: registered but not named in mcp/README.md")

    # The other direction: a table row naming a tool that no longer exists. Only the first cell of a row
    # is checked, because the prose legitimately names argument names like `components_all`.
    stale: list[str] = []
    row_names: set[str] = set()
    if readme.exists():
        for line in text.splitlines():
            match = re.match(r"^\|\s*`([a-z][a-z0-9_]*)`", line)
            if match:
                row_names.add(match.group(1))
                if match.group(1) not in seen:
                    stale.append(match.group(1))
    for name in stale:
        print(f"{name}: in a README tool row but not registered")

    print(f"{len(seen)} MCP tools, {len(duplicates)} duplicate name(s), "
          f"{len(undocumented)} not in the README, {len(row_names)} documented rows, "
          f"{len(stale)} stale row(s)")
    return 1 if duplicates or undocumented or stale else 0


if __name__ == "__main__":
    sys.exit(main())