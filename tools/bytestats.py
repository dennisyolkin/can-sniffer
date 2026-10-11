"""Per-ID, per-byte behavior: which bytes are constant, flags, counters, checksums or signals.

Usage: uv run --project tools tools/bytestats.py <session_dir> [ID ...]

Legend for each byte:
  --          constant (value shown in the per-ID header)
  F:mask      few values (enum or flags); mask = bits that ever change
  CNT/n+s     rolling counter (n = nibble lo/hi or full byte, s = step) between consecutive frames
  CHK         changes in almost every frame by large random amounts: checksum, CRC or noise
  S[lo..hi]   signal: many values, mostly small steps; range shown
  ?[lo..hi]   many values, jumpy; could be part of a wider field
"""

import sys

import numpy as np
import pandas as pd

from canlog import fmt_id, load_frames, payload_bytes


def consecutive_pairs(t: np.ndarray) -> np.ndarray:
    """Mask over diff()s that are true neighbors: frames with no logger stall between them."""
    dt = np.diff(t)
    if len(dt) == 0:
        return np.zeros(0, dtype=bool)
    period = np.median(dt[dt < 150_000]) if (dt < 150_000).any() else np.inf
    return dt < period * 1.5


def counter_step(values: np.ndarray, pairs: np.ndarray, modulus: int) -> int | None:
    d = (np.diff(values.astype(np.int64)) % modulus)[pairs]
    if len(d) < 20:
        return None
    steps, counts = np.unique(d, return_counts=True)
    best = steps[counts.argmax()]
    if best != 0 and counts.max() / len(d) > 0.9:
        return int(best)
    return None


def classify(col: np.ndarray, pairs: np.ndarray) -> str:
    uniq = np.unique(col)
    if len(uniq) == 1:
        return "--"

    for modulus, name, vals in ((256, "8", col), (16, "lo", col & 0x0F), (16, "hi", col >> 4)):
        step = counter_step(vals, pairs, modulus)
        if step is not None:
            return f"CNT/{name}+{step}"

    if len(uniq) <= 8:
        mask = int(np.bitwise_or.reduce(col ^ col[0]))
        return f"F:{mask:08b}"

    d = np.abs(np.diff(col.astype(np.int64)))[pairs]
    if len(d) == 0:
        return f"?[{col.min()}..{col.max()}]"
    changed = (d != 0).mean()
    if changed > 0.9 and np.median(d) > 40:
        return "CHK"
    if np.median(d[d != 0]) <= 4 if (d != 0).any() else True:
        return f"S[{col.min()}..{col.max()}]"
    return f"?[{col.min()}..{col.max()}]"


def main() -> None:
    session = sys.argv[1]
    only = {int(x, 16) for x in sys.argv[2:]}
    frames = load_frames(session)
    data = payload_bytes(frames)

    for (can_id, ext), idx in sorted(frames.groupby(["id", "ext"]).indices.items()):
        if only and can_id not in only:
            continue
        g = frames.iloc[idx]
        dlc = int(g.dlc.max())
        rows = data[idx]
        pairs = consecutive_pairs(g.t.to_numpy())
        const = " ".join(f"{rows[0, i]:02X}" if len(np.unique(rows[:, i])) == 1 else ".." for i in range(dlc))
        cells = [f"b{i}:{classify(rows[:, i], pairs)}" for i in range(dlc)]
        print(f"{fmt_id(can_id, ext)}  n={len(g):<6} [{const}]")
        print("     " + "  ".join(c for c in cells if not c.endswith(":--")))


if __name__ == "__main__":
    main()
