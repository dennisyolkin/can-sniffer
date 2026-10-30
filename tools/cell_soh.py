"""Per-cell SoH from a TX mode session (BMS DID D4B5), saved as CSV and compared with an earlier reading.

Usage: uv run --project tools tools/cell_soh.py <session_dir> [--out cells.csv] [--compare earlier.csv]

Takes the last complete D4B5 reply (`uds_msg bms_d4b5 …` in events.csv): 108 cells × 2 bytes, big-endian,
÷16 = % (the scale of the pack SoH DID D860; see docs/signals.md). Also records the pack SoH (D860) and the
odometer (broadcast `552`) when the session has them, so two readings can be put side by side.
"""

import argparse
import csv
import statistics
from pathlib import Path

from canlog import load_frames, payload_bytes

CELLS = 108


def read_events(session: Path) -> tuple[list[int], float | None]:
    cells, pack = None, None
    with open(session / "events.csv") as f:
        for row in csv.reader(f):
            if len(row) < 3:
                continue
            if row[1] == "uds_msg" and row[2].startswith("bms_d4b5 "):
                data = bytes.fromhex(row[2].split()[1])
                if data[:3] == b"\x62\xd4\xb5" and len(data) == 3 + 2 * CELLS:
                    cells = [int.from_bytes(data[3 + 2 * i:5 + 2 * i], "big") for i in range(CELLS)]
            elif row[1] == "uds_rx" and row[2].startswith("694#0662D860"):
                # Single frame: 06 62 D8 60, then 3 value bytes; SoH is the last two.
                pack = int(row[2][16:20], 16) / 16
    if cells is None:
        raise SystemExit(f"{session}: no complete D4B5 reply in events.csv")
    return cells, pack


def odometer_km(session: Path) -> int | None:
    frames = load_frames(session)
    rows = payload_bytes(frames[frames.id == 0x552])
    if len(rows) == 0:
        return None
    last = rows[-1]
    return (int(last[4]) << 16) | (int(last[5]) << 8) | int(last[6])


def load_csv(path: Path) -> tuple[dict[str, str], list[float]]:
    meta, cells = {}, []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                key, _, value = line[1:].strip().partition("=")
                meta[key.strip()] = value.strip()
            elif line[0].isdigit():
                cells.append(float(line.split(",")[2]))
    return meta, cells


def summary(cells: list[float]) -> str:
    return (f"min {min(cells):.2f}  mean {statistics.mean(cells):.2f}  max {max(cells):.2f}  "
            f"sd {statistics.pstdev(cells):.2f} %")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--out", help="CSV to write (default: <session>/cell_soh.csv)")
    parser.add_argument("--compare", help="an earlier CSV written by this script")
    args = parser.parse_args()

    session = Path(args.session)
    raw, pack = read_events(session)
    km = odometer_km(session)
    soh = [r / 16 for r in raw]

    out = Path(args.out) if args.out else session / "cell_soh.csv"
    with open(out, "w") as f:
        f.write(f"# session = {session.name}\n")
        f.write(f"# odometer_km = {km if km is not None else ''}\n")
        f.write(f"# pack_soh_pct = {f'{pack:.2f}' if pack is not None else ''}\n")
        f.write("# source = BMS DID D4B5, 108 x uint16 big-endian, soh_pct = raw / 16\n")
        f.write("cell,raw,soh_pct\n")
        for i, (r, s) in enumerate(zip(raw, soh), start=1):
            f.write(f"{i},{r},{s:.4f}\n")
    print(f"wrote {out}")
    print(f"odometer {km} km, pack SoH {pack} %")
    print(f"cells: {summary(soh)}")
    ranked = sorted(range(CELLS), key=lambda i: soh[i])
    print("weakest:", ", ".join(f"#{i + 1} {soh[i]:.2f}" for i in ranked[:5]))
    print("strongest:", ", ".join(f"#{i + 1} {soh[i]:.2f}" for i in ranked[-5:]))

    if args.compare:
        meta, old = load_csv(Path(args.compare))
        if len(old) != CELLS:
            raise SystemExit(f"{args.compare}: expected {CELLS} cells, got {len(old)}")
        delta = [n - o for n, o in zip(soh, old)]
        print(f"\ncompared with {meta.get('session', args.compare)} "
              f"(odometer {meta.get('odometer_km') or '?'} km, pack SoH {meta.get('pack_soh_pct') or '?'} %)")
        print(f"then:  {summary(old)}")
        print(f"now:   {summary(soh)}")
        print(f"change per cell: mean {statistics.mean(delta):+.2f}, min {min(delta):+.2f}, "
              f"max {max(delta):+.2f} points")
        worst = sorted(range(CELLS), key=lambda i: delta[i])[:5]
        print("largest drops:", ", ".join(f"#{i + 1} {old[i]:.2f} -> {soh[i]:.2f}" for i in worst))


if __name__ == "__main__":
    main()
