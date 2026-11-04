"""Energy drawn and regenerated over a capture, as the live page's "This trip" panel counts it.

Usage: uv run --project tools tools/trip_energy.py <session_dir>

Two independent sources, so the 3A8 unit can be checked after a drive:
- 3A8 counters (1 byte each, wrapping): b1 = drawn, b2 = regenerated (counts down), b0 = net. 1 Wh per unit, moving
  in 2 Wh steps (trip-20261104; PSA-RE says 2 Wh per unit). The car resets them to 0 / 0 / 254 at the start of each
  drive cycle; that jump is not energy and is skipped.
- 563 remaining Ah × 5C3 pack voltage, integrated frame by frame (falling Ah = drawn, rising = regenerated).
A 3A8 gap longer than 15 s is reported: at full power a counter can wrap in about that time.
"""

import argparse

import numpy as np

from canlog import load_frames, payload_bytes

WH_PER_STEP = 1
GAP_S = 15
MAX_AH_STEP = 1  # 563 glitches by tens of Ah in single frames (trip3)


RESET = (0, 0, 254)


def wrap_delta(now: np.ndarray, before: np.ndarray) -> np.ndarray:
    return (now - before + 384) % 256 - 128


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    args = parser.parse_args()

    frames = load_frames(args.session)
    t0 = frames.t.iloc[0]

    f = frames[frames.id == 0x3A8]
    b = payload_bytes(f).astype(np.int64)
    t = (f.t.to_numpy() - t0) / 1e6
    reset = (b[1:, :3] == RESET).all(axis=1) & (b[:-1, :3] != b[1:, :3]).any(axis=1)
    dn = np.where(reset, 0, wrap_delta(b[1:, 0], b[:-1, 0]))
    d1 = np.where(reset, 0, wrap_delta(b[1:, 1], b[:-1, 1]))
    d2 = np.where(reset, 0, wrap_delta(b[1:, 2], b[:-1, 2]))
    net = dn.sum()
    drawn, regen = d1[d1 > 0].sum(), -d2[d2 < 0].sum()
    gaps = int(np.sum(np.diff(t) > GAP_S))

    f = frames[frames.id == 0x563]
    r = payload_bytes(f).astype(np.int64)
    ah = ((r[:, 4] << 12) | (r[:, 5] << 4) | (r[:, 6] >> 4)) / 1000
    valid = (ah > 0) & (ah < 1048.575)
    ah = ah[valid]
    ah_t = f.t.to_numpy()[valid]
    v = frames[frames.id == 0x5C3]
    vb = payload_bytes(v).astype(np.int64)
    volts = ((vb[:, 0] << 3) | (vb[:, 1] >> 5)) * 0.25
    volts_at = np.interp(ah_t[1:], v.t.to_numpy(), volts)
    dah = np.diff(ah)
    ok = np.abs(dah) <= MAX_AH_STEP
    wh_out = float(np.sum(-dah[ok & (dah < 0)] * volts_at[ok & (dah < 0)]))
    wh_in = float(np.sum(dah[ok & (dah > 0)] * volts_at[ok & (dah > 0)]))

    odo = payload_bytes(frames[frames.id == 0x552]).astype(np.int64)
    km = None
    if len(odo):
        km = int((odo[-1, 4] << 16 | odo[-1, 5] << 8 | odo[-1, 6]) - (odo[0, 4] << 16 | odo[0, 5] << 8 | odo[0, 6]))

    print(f"{args.session}: {(frames.t.iloc[-1] - t0) / 1e6:.0f} s" + (f", {km} km" if km is not None else ""))
    print(f"3A8 steps: drawn {drawn}, regenerated {regen}, net {net} (drawn - regen = {drawn - regen})"
          f"{f', {gaps} gap(s) > {GAP_S} s' if gaps else ''}, {int(reset.sum())} drive-cycle reset(s)")
    if drawn:
        print(f"  recovered {regen / drawn * 100:.1f} % of drawn")
    print(f"  at {WH_PER_STEP} Wh/step: drawn {drawn * WH_PER_STEP / 1000:.2f}, regen {regen * WH_PER_STEP / 1000:.2f}, "
          f"net {net * WH_PER_STEP / 1000:.2f} kWh")
    print(f"563 x 5C3: drawn {wh_out / 1000:.2f}, regen {wh_in / 1000:.2f}, net {(wh_out - wh_in) / 1000:.2f} kWh"
          f" ({int(np.sum(~ok))} Ah step(s) > {MAX_AH_STEP} Ah skipped)")
    if net > 0 and wh_out > wh_in:
        print(f"  implied 3A8 unit: {(wh_out - wh_in) / net:.2f} Wh/step (net) and {wh_in / regen:.2f} Wh/step (regen)"
              if regen else f"  implied 3A8 unit: {(wh_out - wh_in) / net:.2f} Wh/step (net)")


if __name__ == "__main__":
    main()
