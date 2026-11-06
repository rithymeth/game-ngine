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
    seen = collect(Path(args.root))
    duplicates = {name: files for name, files in seen.items() if len(files) > 1}
    for name, files in sorted(duplicates.items()):
        print(f"{name}: registered by {', '.join(files)}")
    print(f"{len(seen)} MCP tools, {len(duplicates)} duplicate name(s)")
    return 1 if duplicates else 0


if __name__ == "__main__":
    sys.exit(main())