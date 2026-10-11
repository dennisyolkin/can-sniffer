"""Capture health and per-ID inventory.

Usage: uv run --project tools tools/inventory.py <session_dir>
"""

import sys

import numpy as np
import pandas as pd

from canlog import fmt_id, load_events, load_frames, payload_bytes

# Gaps longer than this between consecutive frames of the whole bus are logger stalls, not silence:
# the busiest IDs repeat every 10 ms.
STALL_US = 50_000


def health(frames: pd.DataFrame, events: pd.DataFrame) -> None:
    dur = (frames.t.iloc[-1] - frames.t.iloc[0]) / 1e6
    dropped = events.loc[events.event == "rx_dropped_total", "value"].astype(int)
    dropped = int(dropped.max()) if len(dropped) else 0
    errors = events[events.event == "bus_errors"]
    gaps = np.diff(frames.t.to_numpy())
    stalls = gaps[gaps > STALL_US]

    print(f"frames logged   {len(frames):,} over {dur:.1f} s ({len(frames) / dur:.0f}/s)")
    print(f"frames dropped  {dropped:,}  -> captured {len(frames) / (len(frames) + dropped) * 100:.1f}%")
    print(f"bus error lines {len(errors)}" + (f", last: {errors.value.iloc[-1]}" if len(errors) else ""))
    print(f"logger stalls   {len(stalls)} (> {STALL_US // 1000} ms), {stalls.sum() / 1e6 / dur * 100:.1f}% of time")
    print(f"extended IDs    {int(frames.ext.sum())}, RTR {int(frames.rtr.sum())}")


def inventory(frames: pd.DataFrame) -> pd.DataFrame:
    start, end = frames.t.iloc[0], frames.t.iloc[-1]
    data = payload_bytes(frames)
    rows = []
    for (can_id, ext), idx in frames.groupby(["id", "ext"]).indices.items():
        g = frames.iloc[idx]
        dt = np.diff(g.t.to_numpy())
        # Period from gaps inside capture windows only; across a stall the gap is meaningless.
        dt = dt[dt < STALL_US * 3]
        payloads = {bytes(r[:d]) for r, d in zip(data[idx], g.dlc.to_numpy())}
        rows.append({
            "id": fmt_id(can_id, ext),
            "n": len(g),
            "dlc": int(g.dlc.mode().iloc[0]),
            "dlc_var": g.dlc.nunique() > 1,
            "uniq": len(payloads),
            "period_ms": round(float(np.median(dt)) / 1e3, 1) if len(dt) else None,
            "first_s": round((g.t.iloc[0] - start) / 1e6, 1),
            "last_s": round((g.t.iloc[-1] - start) / 1e6, 1),
            "until_end_s": round((end - g.t.iloc[-1]) / 1e6, 1),
        })
    return pd.DataFrame(rows).sort_values("id")


def main() -> None:
    session = sys.argv[1]
    frames = load_frames(session)
    health(frames, load_events(session))
    print()
    pd.set_option("display.width", 200)
    pd.set_option("display.max_rows", 1000)
    print(inventory(frames).to_string(index=False))


if __name__ == "__main__":
    main()
