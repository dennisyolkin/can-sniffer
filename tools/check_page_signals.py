"""Decode a capture with the live page's signal table, to check it without the car.

Usage: uv run --project tools tools/check_page_signals.py <session_dir> [--page main/web/index.html]

The table is the JSON block in the page (<script id="signals">), in PSA-RE bit notation, so this uses the same bit
extraction as psare.py. For every signal it prints first / last / min / max of the physical value (or the enum
distribution), which shows a wrong bit range or scale at a glance.
"""

import argparse
import collections
import json
import re
from pathlib import Path

import numpy as np

from canlog import load_frames, payload_bytes
from psare import bit_positions, raw_values

DEFAULT_PAGE = Path(__file__).resolve().parent.parent / "main" / "web" / "index.html"


def load_table(page: Path) -> dict:
    m = re.search(r'<script type="application/json" id="signals">(.*?)</script>', page.read_text(), re.S)
    if m is None:
        raise SystemExit(f"no signal table in {page}")
    return json.loads(m[1])


def physical(spec: dict, raw: np.ndarray, width: int) -> np.ndarray:
    """Physical values as floats, NaN where the raw value is marked invalid."""
    inv = spec.get("inv", [])
    inv = inv if isinstance(inv, list) else [inv]
    bad = np.isin(raw, inv)
    if spec.get("t") == "s":
        raw = np.where(raw >= 1 << (width - 1), raw - (1 << width), raw)
    out = raw.astype(float)
    if spec.get("t") not in ("enum", "bool"):
        out = out * spec.get("f", 1) + spec.get("o", 0)
    out[bad] = np.nan
    return out


def describe(spec: dict, values: np.ndarray) -> str:
    valid = values[~np.isnan(values)]
    invalid = f"  invalid {1 - len(valid) / len(values):.0%}" if len(valid) < len(values) else ""
    if len(valid) == 0:
        return "all invalid"
    if "enum" in spec:
        counts = collections.Counter(valid.astype(int).tolist())
        parts = [f"{spec['enum'].get(str(v), v)} {c / len(valid):.0%}" for v, c in counts.most_common(5)]
        return "; ".join(parts) + invalid
    dec = spec.get("dec", 2)
    fmt = lambda x: f"{x:.{dec}f}"
    return (f"first {fmt(valid[0])} last {fmt(valid[-1])} min {fmt(valid.min())} max {fmt(valid.max())}"
            f"  ({len(np.unique(valid))} values){invalid}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--page", type=Path, default=DEFAULT_PAGE)
    args = parser.parse_args()

    table = load_table(args.page)
    frames = load_frames(args.session)
    data = payload_bytes(frames)
    groups = frames.groupby("id").indices

    for spec in table["signals"]:
        can_id = int(spec["id"], 16)
        label = f"{spec['k']:12} {spec['id']} {spec['bits']:>8}  [{spec.get('conf', '')}]"
        if can_id not in groups:
            print(f"{label}  no frames")
            continue
        rows = data[groups[can_id]]
        width = len(bit_positions(spec["bits"]))
        print(f"{label}  {describe(spec, physical(spec, raw_values(rows, spec), width))}")

    for group in table["groups"]:
        can_id = int(group["id"], 16)
        if can_id not in groups:
            print(f"{group['k']:12} {group['id']}  no frames")
            continue
        rows = data[groups[can_id]]
        pages = raw_values(rows, {"bits": group["page"]})
        seen = sorted(set(pages.tolist()))
        values = []
        for field in group["fields"]:
            width = len(bit_positions(field))
            values.append(physical(group, raw_values(rows, {"bits": field}), width))
        values = np.concatenate(values)
        valid = values[~np.isnan(values)]
        rng = f"min {valid.min():.3f} max {valid.max():.3f}" if len(valid) else "all invalid"
        extra = sorted(p for p in seen if p >= group["pages"])
        print(f"{group['k']:12} {group['id']}  pages {len(seen)} (expected {group['pages']})"
              f"{', unexpected ' + str(extra) if extra else ''}  {rng}")


if __name__ == "__main__":
    main()
