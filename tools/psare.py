"""Decode a capture with frame definitions from the PSA-RE project (github.com/prototux/PSA-RE).

Usage: uv run --project tools tools/psare.py <session_dir> <psa-re checkout> [--arch AEE2010]
       [--variant electric] [--bus HS.HS1] [--ids 3A8,3F4]

Frames are looked up in buses/<arch>.<variant>/<bus>/, then in the variant's parents (from
architectures.yml), so overrides in a child variant win. For every signal it prints first / last /
min / max of the physical value over the capture, which is a quick way to see whether a definition
fits this car: a wrong one usually gives absurd ranges.

Bit notation (PSA-RE's dbmuxev format): '<byte>.<bit>', bytes from 1, bits 7 (MSB) to 0; a range
'a.b-c.d' is read in transmission order, big-endian unless byte_order is little_endian.
"""

import argparse
import collections
from pathlib import Path

import numpy as np
import yaml

from canlog import load_frames, payload_bytes


def variant_chain(repo: Path, arch: str, variant: str) -> list[str]:
    archs = yaml.safe_load((repo / "architectures.yml").read_text())
    chain = []
    while variant:
        chain.append(f"{arch}.{variant}")
        variant = archs[arch][variant].get("parent")
    return chain


def load_definitions(repo: Path, chain: list[str], bus: str) -> dict[int, dict]:
    defs = {}
    # Parents first, so children overwrite them.
    for name in reversed(chain):
        bus_dir = repo / "buses" / name / bus
        if not bus_dir.is_dir():
            continue
        for path in sorted(bus_dir.glob("*.yml")):
            frame = yaml.safe_load(path.read_text())
            if frame.get("type", "can") == "can":
                frame["_source"] = f"{name}/{bus}/{path.name}"
                defs[int(frame["id"])] = frame
    return defs


def bit_positions(spec: str) -> list[int]:
    """Absolute bit positions (0 = MSB of byte 1) in transmission order."""
    def pos(s: str) -> int:
        byte, bit = s.split(".")
        return (int(byte) - 1) * 8 + (7 - int(bit))
    if "-" not in spec:
        return [pos(spec)]
    start, end = spec.split("-")
    return list(range(pos(start), pos(end) + 1))


def raw_values(data: np.ndarray, sig: dict) -> np.ndarray | None:
    if sig.get("byte_order") == "little_endian":
        positions = bit_positions(sig["bits"])
        if positions[0] % 8 or (positions[-1] + 1) % 8:
            return None  # Not byte-aligned; no such case worth supporting yet.
        cols = list(range(positions[0] // 8, (positions[-1] + 1) // 8))
        value = np.zeros(len(data), dtype=np.int64)
        for k, c in enumerate(cols):
            value |= data[:, c].astype(np.int64) << (8 * k)
        return value
    bits = np.unpackbits(data, axis=1)
    value = np.zeros(len(data), dtype=np.int64)
    for p in bit_positions(sig["bits"]):
        value = (value << 1) | bits[:, p]
    return value


def invalid_set(sig: dict) -> set[int]:
    inv = sig.get("invalid")
    if inv is None:
        return set()
    return set(inv) if isinstance(inv, list) else {inv}


def describe(sig: dict, raw: np.ndarray) -> str:
    kind = sig.get("type", "uint")
    width = len(bit_positions(sig["bits"]))
    if kind in ("str", "bytes", "float", "bcd"):
        return f"({kind}, not decoded)"
    if kind == "sint":
        raw = np.where(raw >= 1 << (width - 1), raw - (1 << width), raw)
    valid = raw[~np.isin(raw, list(invalid_set(sig)))]
    n_invalid = len(raw) - len(valid)
    inv = f"  invalid {n_invalid / len(raw):.0%}" if n_invalid else ""
    if len(valid) == 0:
        return f"all invalid"
    if kind in ("enum", "bool"):
        labels = sig.get("values") or {}
        counts = collections.Counter(valid.tolist())
        parts = []
        for v, c in counts.most_common(4):
            label = labels.get(v, {}).get("en", "") if isinstance(labels.get(v), dict) else ""
            parts.append(f"{v:#x}{'=' + label if label else ''} {c / len(valid):.0%}")
        more = f" (+{len(counts) - 4} more)" if len(counts) > 4 else ""
        return "; ".join(parts) + more + inv
    phys = valid * sig.get("factor", 1) + sig.get("offset", 0)
    units = sig.get("units", "")
    fmt = lambda x: f"{x:.6g}"
    return (f"first {fmt(phys[0])} last {fmt(phys[-1])} min {fmt(phys.min())} max {fmt(phys.max())} {units}"
            f"  ({len(np.unique(phys))} values){inv}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("repo", type=Path)
    parser.add_argument("--arch", default="AEE2010")
    parser.add_argument("--variant", default="electric")
    parser.add_argument("--bus", default="HS.HS1")
    parser.add_argument("--ids", help="comma-separated hex IDs to decode (default: all)")
    args = parser.parse_args()

    chain = variant_chain(args.repo, args.arch, args.variant)
    defs = load_definitions(args.repo, chain, args.bus)
    frames = load_frames(args.session)
    data = payload_bytes(frames)
    groups = frames.groupby("id").indices
    wanted = {int(x, 16) for x in args.ids.split(",")} if args.ids else None

    seen = sorted(i for i in groups if i in defs)
    unknown = sorted(i for i in groups if i not in defs)
    print(f"definitions: {' > '.join(chain)} / {args.bus}: {len(defs)} frames")
    print(f"capture IDs documented: {len(seen)}/{len(groups)}; undocumented: {' '.join(f'{i:03X}' for i in unknown)}")

    for can_id in seen:
        if wanted and can_id not in wanted:
            continue
        frame = defs[can_id]
        idx = groups[can_id]
        rows = data[idx]
        dlc = int(frames.dlc.to_numpy()[idx].max())
        mismatch = "" if frame.get("length") == dlc else f"  ** length {frame.get('length')} in PSA-RE, {dlc} here"
        print(f"\n{can_id:03X} {frame.get('name')}  [{frame['_source']}]{mismatch}")
        comment = (frame.get("comment") or {}).get("en")
        if comment:
            print(f"    {comment}")

        sigs = {k: v for k, v in (frame.get("signals") or {}).items() if not v.get("unused")}
        raws = {}
        for name, sig in sigs.items():
            try:
                raws[name] = raw_values(rows, sig)
            except (KeyError, ValueError, IndexError):
                raws[name] = None
        for name, sig in sigs.items():
            raw = raws[name]
            if raw is None:
                print(f"    {name:40} (cannot decode bits {sig.get('bits')})")
                continue
            mux = sig.get("mux")
            if mux:
                sel = raws.get(mux["selector"])
                if sel is None:
                    continue
                raw = raw[np.isin(sel, mux["values"])]
                if len(raw) == 0:
                    continue
            print(f"    {name:40} {sig['bits']:>9}  {describe(sig, raw)}")


if __name__ == "__main__":
    main()
