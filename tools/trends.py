"""Find slow, steadily rising or falling fields (odometer, SoC, energy, temperatures).

Usage: uv run --project tools tools/trends.py <session_dir> [--top N]

Every 8-, 16- and 24-bit big-endian field is resampled to 10 s medians and ranked by Spearman
correlation with time. Fields with fewer than 3 distinct values are skipped (flags, not trends).
"""

import argparse

import numpy as np
import pandas as pd

from canlog import fmt_id, load_frames, payload_bytes

BIN_US = 10_000_000


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--top", type=int, default=50)
    args = parser.parse_args()

    frames = load_frames(args.session)
    data = payload_bytes(frames)
    t0 = frames.t.iloc[0]

    results = []
    for cid, idx in frames.groupby("id").indices.items():
        bins = (frames.t.to_numpy()[idx] - t0) // BIN_US
        rows = data[idx].astype(np.int64)
        dlc = int(frames.dlc.to_numpy()[idx].max())
        fields = [(f"{i}", rows[:, i]) for i in range(dlc)]
        fields += [(f"{i}:2", (rows[:, i] << 8) | rows[:, i + 1]) for i in range(dlc - 1)]
        fields += [(f"{i}:3", (rows[:, i] << 16) | (rows[:, i + 1] << 8) | rows[:, i + 2]) for i in range(dlc - 2)]
        for name, v in fields:
            s = pd.Series(v).groupby(bins).median()
            if len(s) < 20 or s.nunique() < 3:
                continue
            # Spearman = Pearson on ranks; done by hand to avoid a scipy dependency.
            rho = np.corrcoef(np.arange(len(s)), s.rank().to_numpy())[0, 1]
            results.append((f"{fmt_id(cid)}:{name}", rho, int(s.iloc[0]), int(s.iloc[-1]),
                            int(s.min()), int(s.max()), s.nunique()))

    out = pd.DataFrame(results, columns=["field", "rho", "first", "last", "min", "max", "levels"])
    out = out.reindex(out.rho.abs().sort_values(ascending=False).index)
    pd.set_option("display.width", 200)
    print(out.head(args.top).to_string(index=False, float_format=lambda x: f"{x:.3f}"))


if __name__ == "__main__":
    main()
