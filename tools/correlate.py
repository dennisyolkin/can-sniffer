"""Rank every 8- and 16-bit field on the bus by correlation with a reference field.

Usage: uv run --project tools tools/correlate.py <session_dir> REF [--top N]

REF uses plot.py's field syntax (e.g. 30D:0:2). All fields are resampled to the median value per
1 s bin, so IDs with different periods and the logger's capture gaps line up.
"""

import argparse

import numpy as np
import pandas as pd

from canlog import fmt_id, load_frames, payload_bytes
from plot import extract, parse_field

BIN_US = 1_000_000


def binned(t_us: np.ndarray, v: np.ndarray, bins: np.ndarray) -> pd.Series:
    return pd.Series(v, index=np.searchsorted(bins, t_us, side="right")).groupby(level=0).median()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("ref")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()

    frames = load_frames(args.session)
    data = payload_bytes(frames)
    t0 = frames.t.iloc[0]
    bins = np.arange(t0, frames.t.iloc[-1] + BIN_US, BIN_US)

    can_id, start, nbytes, little, _, signed = parse_field(args.ref)
    t_ref, v_ref = extract(frames, data, can_id, start, nbytes, little, signed)
    ref = binned((t_ref * 1e6 + t0).astype(np.int64), v_ref, bins)

    results = []
    for cid, idx in frames.groupby("id").indices.items():
        g_t = frames.t.to_numpy()[idx]
        rows = data[idx].astype(np.int64)
        dlc = int(frames.dlc.to_numpy()[idx].max())
        fields = [(f"{i}", rows[:, i]) for i in range(dlc)]
        fields += [(f"{i}:2", (rows[:, i] << 8) | rows[:, i + 1]) for i in range(dlc - 1)]
        fields += [(f"{i}:2:le", rows[:, i] | (rows[:, i + 1] << 8)) for i in range(dlc - 1)]
        for name, v in fields:
            if np.all(v == v[0]):
                continue
            s = binned(g_t, v, bins)
            joined = pd.concat([ref, s], axis=1, join="inner").dropna()
            if len(joined) < 30 or joined.iloc[:, 1].nunique() < 3:
                continue
            r = joined.iloc[:, 0].corr(joined.iloc[:, 1])
            # Least-squares slope tells the scale relative to the reference.
            slope = np.polyfit(joined.iloc[:, 0], joined.iloc[:, 1], 1)[0]
            results.append((f"{fmt_id(cid)}:{name}", r, slope, len(joined)))

    out = pd.DataFrame(results, columns=["field", "r", "slope_vs_ref", "bins"])
    out = out.reindex(out.r.abs().sort_values(ascending=False).index)
    print(out.head(args.top).to_string(index=False, float_format=lambda x: f"{x:.4f}"))


if __name__ == "__main__":
    main()
