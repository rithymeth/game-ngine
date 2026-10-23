#!/usr/bin/env python3
"""Check benchmark baseline integrity before regression comparison."""
import json
import math
from pathlib import Path

def validate(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("results"), list) or not data["results"]:
        raise ValueError("baseline requires nonempty results")
    names = set()
    for row in data["results"]:
        name = row["name"]
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("invalid or duplicate benchmark name")
        names.add(name)
        for field in ("min_ms", "median_ms", "p95_ms", "max_ms", "normalized"):
            value = row[field]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError("invalid timing: " + name + "/" + field)
        if not (row["min_ms"] <= row["median_ms"] <= row["p95_ms"] <= row["max_ms"]):
            raise ValueError("unordered timings: " + name)
    return len(names)

if __name__ == "__main__":
    print("Validated", validate("bench/baseline.json"), "benchmark baselines")
