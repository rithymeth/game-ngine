#!/usr/bin/env python3
"""Report the explicit production-gate table without treating unknown as success."""

import argparse
import json
import sys
from pathlib import Path


VALID = {"pass", "fail", "n/a yet"}


def read_gates(path: Path):
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("version") != 1:
        raise ValueError("gate file must be an object with version 1")
    gates = document.get("gates")
    if not isinstance(gates, list) or not gates:
        raise ValueError("gate file must contain a non-empty gates list")
    names = set()
    for gate in gates:
        if not isinstance(gate, dict) or not isinstance(gate.get("name"), str) or not gate["name"]:
            raise ValueError("each gate needs a non-empty name")
        if gate["name"] in names:
            raise ValueError(f"duplicate gate: {gate['name']}")
        names.add(gate["name"])
        if gate.get("status") not in VALID:
            raise ValueError(f"{gate['name']}: status must be one of {sorted(VALID)}")
        if not isinstance(gate.get("evidence"), str) or not gate["evidence"]:
            raise ValueError(f"{gate['name']}: evidence is required")
    return gates


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", type=Path, default=Path(__file__).resolve().parents[1] / "docs/design/production_gates.json")
    parser.add_argument("--require-complete", action="store_true", help="fail while any gate remains n/a yet")
    parser.add_argument("--json", action="store_true", help="print a machine-readable report")
    args = parser.parse_args()
    try:
        gates = read_gates(args.table)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"gate table error: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({"gates": gates}, indent=2))
    else:
        for gate in gates:
            print(f"{gate['status'].upper():8} {gate['name']}: {gate['evidence']}")
    failed = any(gate["status"] == "fail" for gate in gates)
    incomplete = any(gate["status"] == "n/a yet" for gate in gates)
    return int(failed or (args.require_complete and incomplete))


if __name__ == "__main__":
    raise SystemExit(main())
