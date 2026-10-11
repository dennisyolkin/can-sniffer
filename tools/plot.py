"""Plot raw fields over time, one subplot per field.

Usage: uv run --project tools tools/plot.py <session_dir> <out.png> FIELD [FIELD ...]

FIELD is ID:byte[:nbytes[:flags]], e.g. 30D:0:2 (bytes 0-1, big-endian), 30D:0:2:le, 262:0 (one byte).
Flags: "le" for little-endian, "s" for two's-complement signed; combine as "les".
Big-endian is the default because it is the usual byte order on PSA buses (to be confirmed).
Add @lo..hi (seconds) to a field to zoom: 30D:0:2@100..300.
"""

import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from canlog import load_frames, payload_bytes


def parse_field(spec: str) -> tuple[int, int, int, bool, tuple[float, float] | None, bool]:
    window = None
    if "@" in spec:
        spec, w = spec.split("@")
        lo, hi = w.split("..")
        window = (float(lo), float(hi))
    parts = spec.split(":")
    can_id, start = int(parts[0], 16), int(parts[1])
    nbytes = int(parts[2]) if len(parts) > 2 else 1
    flags = parts[3] if len(parts) > 3 else ""
    return can_id, start, nbytes, "le" in flags, window, "s" in flags


def extract(frames, data, can_id: int, start: int, nbytes: int, little: bool,
            signed: bool = False) -> tuple[np.ndarray, np.ndarray]:
    mask = (frames.id == can_id).to_numpy()
    rows = data[mask, start:start + nbytes].astype(np.int64)
    order = range(nbytes) if little else range(nbytes - 1, -1, -1)
    value = sum(rows[:, i] << (8 * k) for k, i in enumerate(order))
    if signed:
        sign_bit = 1 << (8 * nbytes - 1)
        value = (value ^ sign_bit) - sign_bit
    t = (frames.t.to_numpy()[mask] - frames.t.iloc[0]) / 1e6
    return t, value


def main() -> None:
    session, out, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
    frames = load_frames(session)
    data = payload_bytes(frames)

    fig, axes = plt.subplots(len(specs), 1, figsize=(14, 2.2 * len(specs)), sharex=True, squeeze=False)
    for ax, spec in zip(axes[:, 0], specs):
        can_id, start, nbytes, little, window, signed = parse_field(spec)
        t, v = extract(frames, data, can_id, start, nbytes, little, signed)
        if window:
            keep = (t >= window[0]) & (t <= window[1])
            t, v = t[keep], v[keep]
        ax.plot(t, v, ".", markersize=1)
        ax.set_ylabel(spec.split("@")[0], rotation=0, ha="right", fontsize=8)
        ax.grid(alpha=0.3)
    axes[-1, 0].set_xlabel("seconds since first frame")
    fig.tight_layout()
    fig.savefig(out, dpi=90)


if __name__ == "__main__":
    main()
