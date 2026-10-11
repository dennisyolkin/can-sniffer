"""Load a can-sniffer session (can.log in candump format + events.csv) into pandas.

Frames come back as one row per frame: t (µs since boot), id (int), ext, rtr, dlc and the payload
as eight uint8 columns b0..b7 (missing bytes are 0; use dlc to tell them apart). The parsed frames
are cached as can.parquet next to can.log, because parsing the text is the slow part.
"""

import re
from pathlib import Path

import numpy as np
import pandas as pd

LINE = re.compile(rb"^\((\d+)\.(\d{6})\) \S+ ([0-9A-F]{3}|[0-9A-F]{8})#(R\d?|[0-9A-F]{0,16})$")


def _parse(log_path: Path) -> tuple[pd.DataFrame, int]:
    t, ids, ext, rtr, dlc = [], [], [], [], []
    payload = []
    bad = 0
    with open(log_path, "rb") as f:
        for line in f:
            m = LINE.match(line.rstrip(b"\n"))
            if m is None:
                # Partial lines and stale card data (see the 64 KB hole in trip1) end up here.
                bad += 1
                continue
            t.append(int(m[1]) * 1_000_000 + int(m[2]))
            ids.append(int(m[3], 16))
            ext.append(len(m[3]) == 8)
            data = m[4]
            if data.startswith(b"R"):
                rtr.append(True)
                dlc.append(int(data[1:]) if len(data) > 1 else 0)
                payload.append(b"")
            else:
                rtr.append(False)
                raw = bytes.fromhex(data.decode())
                dlc.append(len(raw))
                payload.append(raw)

    df = pd.DataFrame({
        "t": np.array(t, dtype=np.int64),
        "id": np.array(ids, dtype=np.uint32),
        "ext": np.array(ext, dtype=bool),
        "rtr": np.array(rtr, dtype=bool),
        "dlc": np.array(dlc, dtype=np.uint8),
    })
    padded = np.frombuffer(b"".join(p.ljust(8, b"\0") for p in payload), dtype=np.uint8).reshape(-1, 8)
    for i in range(8):
        df[f"b{i}"] = padded[:, i]
    return df, bad


def load_frames(session_dir: str | Path) -> pd.DataFrame:
    session_dir = Path(session_dir)
    log_path = session_dir / "can.log"
    cache = session_dir / "can.parquet"
    if cache.exists() and cache.stat().st_mtime >= log_path.stat().st_mtime:
        return pd.read_parquet(cache)
    df, bad = _parse(log_path)
    if bad:
        print(f"note: skipped {bad} unparsable lines in {log_path}")
    df.to_parquet(cache)
    return df


def load_events(session_dir: str | Path) -> pd.DataFrame:
    return pd.read_csv(Path(session_dir) / "events.csv")


def payload_bytes(frames: pd.DataFrame) -> np.ndarray:
    """The b0..b7 columns as an (n, 8) uint8 array."""
    return frames[[f"b{i}" for i in range(8)]].to_numpy(dtype=np.uint8)


def fmt_id(can_id: int, ext: bool = False) -> str:
    return f"{can_id:08X}" if ext else f"{can_id:03X}"
