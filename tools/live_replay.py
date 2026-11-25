"""Serve the live page on the laptop and feed it a recorded capture, to work on the page without the car.

Usage: uv run --project tools tools/live_replay.py <session_dir> [--start 120|end] [--speed 1] [--port 8080]
Then open http://localhost:8080/ (or the laptop's address from the phone, same Wi-Fi).

Speaks the same WebSocket protocol as the firmware (docs/live-view.md): the latest frame per ID (per page for muxed
IDs) every 200 ms, a status JSON every second. "rec on/off" and "mark" are accepted and printed; recording is
simulated. The page is read from disk on every request, so editing main/web/index.html only needs a reload.

A TX mode session's diagnostics (the uds_* lines of events.csv) are replayed too, as the board's "uds" messages
(request list from main/uds_client.c, preprocessed with `cc -E`), so the Faults tab fills as it did in the car. A
page that connects later first gets everything up to that moment. Requests the page sends are only printed.
--start end jumps to the end of the session: the page gets the whole session at once, then the last frames stay put
(used by tools/page/faults_check.mjs and screenshot.mjs).
"""

import argparse
import asyncio
import csv
import json
import re
import struct
import subprocess
import time
import traceback
from pathlib import Path

import numpy as np
from aiohttp import WSMsgType, web

from canlog import load_frames, payload_bytes

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "main" / "web" / "index.html"
UDS_SOURCE = ROOT / "main" / "uds_client.c"
# UDS_CLIENT_LOG_LEN: the board sends its last 16 log entries.
UDS_LOG_LEN = 16
RX_ERRORS = ["sequence", "too_long", "incomplete"]
# Frames before --start that fill the table at once, so the page has e.g. 552 (time counter, odometer) right away.
PREFILL_S = 10
PUSH_PERIOD_S = 0.2
STATUS_EVERY_N_PUSHES = 5
# Same as muxed_ids in main/live_data.c.
MUX_SHIFT = {0x334: 3, 0x3F4: 5}
NO_PAGE = 0xFF


def uds_requests() -> list[dict]:
    """The request table as the board sends it ({name, label, param, danger}), from uds_client.c's macros."""
    src = UDS_SOURCE.read_text()
    table = src[src.index("#define REQUEST(req_name"):src.index("#define REQUEST_COUNT")]
    try:
        out = subprocess.run(["cc", "-E", "-P", "-x", "c", "-"], input="typedef unsigned char uint8_t;\n" + table,
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"no request list ({e}): the page will show replies but no buttons")
        return []
    strings = lambda x: "".join(re.findall(r'"([^"]*)"', x))
    requests = []
    for m in re.finditer(r'\{\.name = ((?:"[^"]*"\s*)+), \.label = ((?:"[^"]*"\s*)+)(.*?)\}', out, re.S):
        param = re.search(r"param_len = (\d+)", m.group(3))
        requests.append({"name": strings(m.group(1)), "label": strings(m.group(2)),
                         "param": int(param.group(1)) if param else 0, "danger": "danger = 1" in m.group(3)
                         or "danger = true" in m.group(3)})
    return requests


class UdsReplay:
    """events.csv's uds_* lines as the board's log entries ({t, kind, req, id, len, data}) and multi-frame messages."""

    def __init__(self, session: str):
        self.events: list[tuple[int, dict, dict | None]] = []  # (t_us, log entry, message or None)
        self.can_mode = "listen_only"
        path = Path(session) / "events.csv"
        if not path.exists():
            return
        pending: list[dict] = []  # entries waiting for the name of their request (it comes with done / msg)
        last_tx = last_rx = 0
        with open(path) as f:
            for t, event, value in csv.reader(f):
                if event == "can_mode":
                    self.can_mode = value
                if not event.startswith("uds_"):
                    continue
                t_us = int(t)
                entry, msg, name = {"t": t_us // 1000, "req": "?", "id": 0, "len": 0, "data": "00" * 8}, None, None
                if event in ("uds_tx", "uds_fc", "uds_rx"):
                    can_id, data = value.split("#")
                    entry.update(kind="rx" if event == "uds_rx" else "tx", id=int(can_id, 16), len=len(data) // 2,
                                 data=data.ljust(16, "0")[:16])
                    if event == "uds_rx":
                        last_rx = entry["id"]
                    else:
                        last_tx = entry["id"]
                elif event == "uds_msg":
                    name, data = value.split()
                    entry.update(kind="message", id=last_rx, len=len(data) // 2)
                    msg = {"t": t_us // 1000, "req": name, "id": last_rx, "len": len(data) // 2, "data": data}
                elif event == "uds_rx_error":
                    name, error = value.split()[:2]
                    entry.update(kind="rx_error", id=last_rx, len=RX_ERRORS.index(error) if error in RX_ERRORS else 3)
                elif event in ("uds_done", "uds_tx_failed", "uds_refused"):
                    name, n = value.split()
                    entry.update(kind={"uds_done": "done", "uds_tx_failed": "tx_failed", "uds_refused": "refused"}[event],
                                 id=last_tx if event != "uds_done" else 0, len=int(n))
                else:
                    continue
                pending.append(entry)
                if name:
                    for e in pending:
                        e["req"] = name
                    pending = []
                self.events.append((t_us, entry, msg))

    def message(self, requests: list[dict], upto: int) -> str:
        """The board's "uds" message once the first `upto` events have happened."""
        log = [e for _, e, _ in self.events[max(0, upto - UDS_LOG_LEN):upto]]
        out = {"type": "uds", "requests": requests, "log": log}
        msgs = [(i, m) for i, (_, _, m) in enumerate(self.events[:upto]) if m]
        if msgs:
            i, m = msgs[-1]
            out["msg"] = {"seq": i + 1, **m}
        return json.dumps(out)


class Replay:
    def __init__(self, session: str, start_s: float | None, speed: float):
        frames = load_frames(session)
        frames = frames[~frames.ext]
        self.t = frames.t.to_numpy()
        self.ids = frames.id.to_numpy().astype(np.uint16)
        self.dlc = frames.dlc.to_numpy()
        self.rtr = frames.rtr.to_numpy()
        self.data = payload_bytes(frames)
        self.speed = speed
        # start_s None: the end of the capture.
        self.origin_us = int(self.t[-1]) if start_s is None else int(self.t[0]) + int(start_s * 1e6)
        self.wall0 = time.monotonic()
        self.pos = int(np.searchsorted(self.t, self.origin_us))
        self.uds = UdsReplay(session)
        self.requests = uds_requests() if self.uds.events else []
        self.uds_pos = self.uds_at(self.origin_us)
        self.uds_out: list[str] = []  # uds messages from the last advance()
        # key -> [id field, page, dlc, count, last_ms, data bytes]
        self.table: dict[tuple[int, int], list] = {}
        self.fps_count = 0
        self.fps = 0
        self.frames_total = 0
        self.rec = "off"
        self.session = ""
        self.marks = 0
        self.sessions = 0
        self.new_trip(0)
        self.trip_last = None
        self.last_mah = None
        self.volts_x4 = 0
        self.apply_frames(int(np.searchsorted(self.t, self.origin_us - PREFILL_S * 1_000_000)), self.pos)

    def uds_at(self, t_us: int) -> int:
        return sum(1 for t, _, _ in self.uds.events if t <= t_us)

    def uds_history(self) -> list[str]:
        """Everything up to now, one message per event, for a page that just connected."""
        return [self.uds.message(self.requests, i) for i in range(1, self.uds_pos + 1)]

    # Same counting as main/trip_energy.c, so the page's "This trip" panel can be tried without the car.
    def new_trip(self, since_ms: int) -> None:
        km = getattr(self, "trip", {}).get("km_now", -1)
        self.trip = {"drawn_wh": 0, "regen_wh": 0, "net_wh": 0, "cycles": 0, "gaps": 0, "since_ms": since_ms,
                     "km_start": km, "km_now": km, "bat_out_wh": 0, "bat_in_wh": 0, "bat_skips": 0}
        self.bat_qmwh = [0, 0]  # out, in; mAh × quarter volts, as on the board

    def count_trip(self, can_id: int, data: bytes) -> None:
        wrap = lambda now, before: (now - before + 384) % 256 - 128
        if can_id == 0x3A8:
            b = tuple(data[:3])
            if self.trip_last is not None and b != self.trip_last:
                if b == (0, 0, 254):
                    self.trip["cycles"] += 1
                else:
                    self.trip["net_wh"] += wrap(b[0], self.trip_last[0])
                    self.trip["drawn_wh"] += max(0, wrap(b[1], self.trip_last[1]))
                    self.trip["regen_wh"] += max(0, -wrap(b[2], self.trip_last[2]))
            self.trip_last = b
        elif can_id == 0x5C3 and len(data) >= 2:
            raw = (data[0] << 3) | (data[1] >> 5)
            if raw != 0x7FF:
                self.volts_x4 = raw
        elif can_id == 0x563 and len(data) >= 7:
            mah = (data[4] << 12) | (data[5] << 4) | (data[6] >> 4)
            if mah in (0, 0xFFFFF) or not self.volts_x4:
                return
            if self.last_mah is not None:
                step = mah - self.last_mah
                if abs(step) > 1000:
                    self.trip["bat_skips"] += 1
                else:
                    self.bat_qmwh[0 if step < 0 else 1] += abs(step) * self.volts_x4
                self.trip["bat_out_wh"] = self.bat_qmwh[0] // 4000
                self.trip["bat_in_wh"] = self.bat_qmwh[1] // 4000
            self.last_mah = mah
        elif can_id == 0x552:
            km = (data[4] << 16) | (data[5] << 8) | data[6]
            if km != 0xFFFFFF:
                self.trip["km_now"] = km
                if self.trip["km_start"] < 0:
                    self.trip["km_start"] = km

    def now_us(self) -> int:
        return self.origin_us + int((time.monotonic() - self.wall0) * self.speed * 1e6)

    def apply_frames(self, begin: int, end: int) -> None:
        for i in range(begin, end):
            can_id = int(self.ids[i])
            data = bytes(self.data[i])
            shift = MUX_SHIFT.get(can_id)
            page = data[0] >> shift if shift is not None and not self.rtr[i] and self.dlc[i] > 0 else NO_PAGE
            entry = self.table.setdefault((can_id, page), [0, page, 0, 0, 0, b""])
            entry[0] = can_id | (0x8000 if self.rtr[i] else 0)
            entry[2] = int(self.dlc[i])
            entry[3] += 1
            entry[4] = int(self.t[i] // 1000) & 0xFFFFFFFF
            entry[5] = data
            self.count_trip(can_id, data)

    def advance(self) -> int:
        now = self.now_us()
        end = int(np.searchsorted(self.t, now, side="right"))
        self.apply_frames(self.pos, end)
        # One uds message per event, so no multi-frame reply or log entry is skipped between pushes.
        while self.uds_pos < len(self.uds.events) and self.uds.events[self.uds_pos][0] <= now:
            self.uds_pos += 1
            self.uds_out.append(self.uds.message(self.requests, self.uds_pos))
        self.fps_count += end - self.pos
        self.frames_total += end - self.pos
        self.pos = end
        # Started at the end (--start end): stay there.
        if self.pos >= len(self.t) and self.origin_us < int(self.t[-1]):
            print("end of capture, starting over")
            self.wall0 = time.monotonic()
            self.pos = int(np.searchsorted(self.t, self.origin_us))
            self.uds_pos = self.uds_at(self.origin_us)
        return now

    def frames_message(self, now_us: int) -> bytes:
        entries = sorted(self.table.values(), key=lambda e: (e[0] & 0x7FF, e[1]))
        out = [struct.pack("<BBHI", ord("F"), 1, len(entries), (now_us // 1000) & 0xFFFFFFFF)]
        for id_field, page, dlc, count, last_ms, data in entries:
            out.append(struct.pack("<HBBII8s", id_field, page, dlc, count, last_ms, data))
        return b"".join(out)

    def status_message(self, now_us: int, clients: int) -> str:
        # Frames per second of capture time, so the header shows the bus rate at any --speed.
        self.fps, self.fps_count = round(self.fps_count / self.speed), 0
        return json.dumps({
            "type": "status", "uptime_ms": now_us // 1000, "fps": self.fps, "frames": self.frames_total,
            "rx_dropped": 0, "bus_errors": 0, "bus_errors_per_s": 0, "rec": self.rec, "session": self.session,
            "sd_dropped": 0, "buffers_free": 7, "buffers": 8, "marks": self.marks,
            "can_mode": self.uds.can_mode, "trip": self.trip, "clients": clients,
            "bus_state": "error_active" if self.uds.can_mode == "tx" else "error_passive",
            "heap_free": 120_000, "heap_min": 100_000,
        })

    def command(self, text: str) -> None:
        if text == "rec on" and self.rec != "on":
            self.sessions += 1
            self.rec, self.session, self.marks = "on", f"replay000-{self.sessions:02d}", 0
        elif text == "rec off":
            self.rec, self.session = "off", ""
        elif text.startswith("mark ") and self.rec == "on":
            self.marks += 1
        elif text == "trip reset":
            self.new_trip(self.now_us() // 1000)
        t_s = self.now_us() / 1e6
        print(f"{t_s:9.1f} s  command: {text}", flush=True)


async def index(request: web.Request) -> web.Response:
    return web.Response(text=PAGE.read_text(), content_type="text/html")


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    for text in request.app["replay"].uds_history():
        await ws.send_str(text)
    request.app["clients"].add(ws)
    request.app["state"]["joined"] = True
    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                request.app["replay"].command(msg.data)
    finally:
        request.app["clients"].discard(ws)
    return ws


async def pusher(app: web.Application) -> None:
    replay: Replay = app["replay"]
    n = 0
    while True:
        await asyncio.sleep(PUSH_PERIOD_S)
        try:
            await push_once(app, replay, n)
            n += 1
        except Exception:
            # An exception would otherwise end the task silently and the page would just see no data.
            traceback.print_exc()


async def push_once(app: web.Application, replay: "Replay", n: int) -> None:
    now = replay.advance()
    uds, replay.uds_out = replay.uds_out, []
    clients = list(app["clients"])
    if not clients:
        return
    frames = replay.frames_message(now)
    status = None
    if (n + 1) % STATUS_EVERY_N_PUSHES == 0 or app["state"]["joined"]:
        app["state"]["joined"] = False
        status = replay.status_message(now, len(clients))
    for ws in clients:
        try:
            await ws.send_bytes(frames)
            if status:
                await ws.send_str(status)
            for text in uds:
                await ws.send_str(text)
        except ConnectionError:
            app["clients"].discard(ws)


async def start_pusher(app: web.Application) -> None:
    app["task"] = asyncio.create_task(pusher(app))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--start", default="0", help="seconds into the capture, or 'end'")
    parser.add_argument("--speed", type=float, default=1)
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    app = web.Application()
    app["replay"] = Replay(args.session, None if args.start == "end" else float(args.start), args.speed)
    app["clients"] = set()
    app["state"] = {"joined": False}
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.on_startup.append(start_pusher)
    web.run_app(app, port=args.port)


if __name__ == "__main__":
    main()
