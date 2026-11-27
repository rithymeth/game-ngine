#!/usr/bin/env python3
"""Render a Jump Envelope receipt as a self-contained SVG timing map."""

import argparse
from html import escape
import json
from pathlib import Path

from jump_envelope import MAX_RADIUS, serialize_tilemap_overlay
from jump_proof_compare import finite_number, write_new_json

MAX_RECEIPT = 16 * 1024 * 1024
LABELS = {"kept": "Kept", "lost": "LOST", "gained": "Gained", "missed": "Missed",
          "skipped": "Not tested", "pass": "Pass"}
COLORS = {"kept": "#137859", "lost": "#ad2739", "gained": "#145bb8", "missed": "#573d65",
          "skipped": "#556479", "pass": "#137859"}


def statuses(build, expected, label):
    table = build.get("trials") if isinstance(build, dict) else None
    if not isinstance(table, list) or len(table) != len(expected):
        raise ValueError(f"{label} does not match the tested offsets")
    result = {}
    for entry in table:
        offset = entry.get("offset") if isinstance(entry, dict) else None
        if (type(offset) is not int or offset not in expected or offset in result
                or entry.get("status") not in ("pass", "missed")):
            raise ValueError(f"{label} does not match the tested offsets")
        result[offset] = entry["status"]
    return result


def label_text(value, limit):
    if not isinstance(value, str):
        raise ValueError("receipt scene and key must be strings")
    value = "".join(char if char.isprintable() else " " for char in value)
    return value if len(value) <= limit else value[:limit - 1] + "…"


def offset_label(offset):
    return f"{offset:+d}" if offset else "0"


def render(receipt):
    if not isinstance(receipt, dict) or receipt.get("schema") != "aether.jump-envelope.experimental.v1":
        raise ValueError("expected a Jump Envelope v1 receipt")
    radius, hz = receipt.get("radius"), receipt.get("fixed_hz")
    if type(radius) is not int or not 1 <= radius <= MAX_RADIUS or not finite_number(hz) or hz <= 0:
        raise ValueError("invalid radius or fixed tick rate")
    offsets = range(-radius, radius + 1)
    skipped = receipt.get("skipped_offsets")
    if (not isinstance(skipped, list) or len(skipped) > 2 * radius
            or any(type(n) is not int or n not in offsets or n == 0 for n in skipped)
            or len(set(skipped)) != len(skipped)):
        raise ValueError("invalid skipped offsets")
    expected = set(offsets) - set(skipped)
    baseline = statuses(receipt.get("baseline"), expected, "baseline")
    compare = "candidate" in receipt
    candidate = statuses(receipt["candidate"], expected, "candidate") if compare else {}

    def result_at(offset):
        if offset not in expected:
            return "skipped"
        if not compare:
            return baseline[offset]
        return {("pass", "pass"): "kept", ("pass", "missed"): "lost",
                ("missed", "pass"): "gained", ("missed", "missed"): "missed"}[baseline[offset], candidate[offset]]

    headline = f"{len(expected)} tested · {len(skipped)} untested"
    lost = {offset for offset in offsets if result_at(offset) == "lost"}
    if compare:
        gained = sum(result_at(offset) == "gained" for offset in offsets)
        headline += f" · {len(lost)} lost · {gained} gained"
        if result_at(0) == "kept":
            headline += " · Original timing still passes"
    evidence = receipt.get("evidence")
    nearest = evidence.get("nearest_lost_replay") if isinstance(evidence, dict) else None
    detail = ""
    if isinstance(nearest, dict) and type(nearest.get("offset")) is int and nearest["offset"] in lost:
        divergence = nearest.get("divergence")
        frame = divergence.get("first_frame") if isinstance(divergence, dict) else None
        suffix = f" (offset {offset_label(nearest['offset'])})"
        if type(frame) is int and 1 <= frame <= 20_000:
            detail = f"First split: frame {frame}" + suffix
        elif isinstance(divergence, dict) and frame is None:
            detail = "No position split above tolerance" + suffix

    height = 210 + 32 * len(offsets)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="680" height="{height}" '
             f'viewBox="0 0 680 {height}" role="img" aria-labelledby="title summary">',
             '<title id="title">Aether Jump Envelope timing map</title>',
             f'<desc id="summary">{escape(headline)}</desc>',
             f'<rect width="680" height="{height}" rx="12" fill="#101927"/>',
             '<g font-family="system-ui, sans-serif">']

    def text(x, y, value, *, size=15, color="#f2f6fb", bold=False):
        weight = ' font-weight="700"' if bold else ""
        parts.append(f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}"{weight}>{escape(value)}</text>')

    text(24, 39, "JUMP ENVELOPE / TIMING MAP", size=24, bold=True)
    scene, key = label_text(receipt.get("scene"), 44), label_text(receipt.get("key"), 16)
    text(24, 67, f"{scene} · {key} · {hz:g} Hz", color="#b8c8dc")
    text(24, 94, headline, bold=True)
    text(24, 121, detail or "Offsets are fixed simulation frames.", color="#b8c8dc")
    columns = [(24, "OFFSET"), (185, "BASELINE"), (340, "CANDIDATE"), (504, "RESULT")]
    for x, name in columns:
        text(x, 156, name, size=13, color="#b8c8dc", bold=True)

    for index, offset in enumerate(offsets):
        top = 167 + index * 32
        state = result_at(offset)
        parts.append(f'<g data-offset="{offset}" data-result="{state}">')
        fill = "#30405b" if not offset else ("#172234" if index % 2 else "#202d42")
        parts.append(f'<rect x="16" y="{top}" width="648" height="30" rx="4" fill="{fill}"/>')
        y = top + 20
        text(24, y, offset_label(offset) + ("  original" if not offset else ""), bold=not offset)
        text(185, y, LABELS[baseline[offset]] if offset in baseline else "—")
        text(340, y, LABELS[candidate[offset]] if offset in candidate else "—")
        parts.append(f'<rect x="493" y="{top + 3}" width="148" height="24" rx="5" fill="{COLORS[state]}"/>')
        text(504, y, LABELS[state], size=13, bold=True)
        parts.append("</g>")
    parts.append("</g></svg>")
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--tilemap-overlay", action="store_true",
                        help="extract the embedded Jump Proof overlay JSON instead of drawing a map")
    parser.add_argument("--output", type=Path, help="write a new file instead of stdout")
    args = parser.parse_args()
    try:
        with args.receipt.open("rb") as source:
            raw = source.read(MAX_RECEIPT + 1)
        if len(raw) > MAX_RECEIPT:
            raise ValueError("receipt exceeds 16 MiB")
        receipt = json.loads(raw.decode("utf-8"))
        if args.tilemap_overlay:
            if not isinstance(receipt, dict) or receipt.get("schema") != "aether.jump-envelope.experimental.v1":
                raise ValueError("expected a Jump Envelope v1 receipt")
            evidence = receipt.get("evidence")
            replay = evidence.get("nearest_lost_replay") if isinstance(evidence, dict) else None
            overlay = replay.get("tilemap_overlay") if isinstance(replay, dict) else None
            if overlay is None:
                raise ValueError("receipt has no lost-timing Tilemap overlay")
            output = serialize_tilemap_overlay(overlay)
        else:
            output = render(receipt)
        if args.output is None:
            print(output)
        else:
            write_new_json(args.output, output)
    except (OSError, ValueError, OverflowError, RecursionError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
