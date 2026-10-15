"""Serve the live page on the laptop and feed it a recorded capture, to work on the page without the car.

Usage: uv run --project tools tools/live_replay.py <session_dir> [--start 120] [--speed 1] [--port 8080]
Then open http://localhost:8080/ (or the laptop's address from the phone, same Wi-Fi).

Speaks the same WebSocket protocol as the firmware (docs/live-view.md): the latest frame per ID (per page for muxed
IDs) every 200 ms, a status JSON every second. "rec on/off" and "mark" are accepted and printed; recording is
simulated. The page is read from disk on every request, so editing main/web/index.html only needs a reload.
"""

import argparse
import asyncio
import json
import struct
import time
import traceback
from pathlib import Path

import numpy as np
from aiohttp import WSMsgType, web

from canlog import load_frames, payload_bytes

PAGE = Path(__file__).resolve().parent.parent / "main" / "web" / "index.html"
PUSH_PERIOD_S = 0.2
STATUS_EVERY_N_PUSHES = 5
# Same as muxed_ids in main/live_data.c.
MUX_SHIFT = {0x334: 3, 0x3F4: 5}
NO_PAGE = 0xFF


class Replay:
    def __init__(self, session: str, start_s: float, speed: float):
        frames = load_frames(session)
        frames = frames[~frames.ext]
        self.t = frames.t.to_numpy()
        self.ids = frames.id.to_numpy().astype(np.uint16)
        self.dlc = frames.dlc.to_numpy()
        self.rtr = frames.rtr.to_numpy()
        self.data = payload_bytes(frames)
        self.speed = speed
        self.origin_us = int(self.t[0]) + int(start_s * 1e6)
        self.wall0 = time.monotonic()
        self.pos = int(np.searchsorted(self.t, self.origin_us))
        # key -> [id field, page, dlc, count, last_ms, data bytes]
        self.table: dict[tuple[int, int], list] = {}
        self.fps_count = 0
        self.fps = 0
        self.frames_total = 0
        self.rec = "off"
        self.session = ""
        self.marks = 0
        self.sessions = 0

    def now_us(self) -> int:
        return self.origin_us + int((time.monotonic() - self.wall0) * self.speed * 1e6)

    def advance(self) -> int:
        now = self.now_us()
        end = int(np.searchsorted(self.t, now, side="right"))
        for i in range(self.pos, end):
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
        self.fps_count += end - self.pos
        self.frames_total += end - self.pos
        self.pos = end
        if self.pos >= len(self.t):
            print("end of capture, starting over")
            self.wall0 = time.monotonic()
            self.pos = int(np.searchsorted(self.t, self.origin_us))
        return now

    def frames_message(self, now_us: int) -> bytes:
        entries = sorted(self.table.values(), key=lambda e: (e[0] & 0x7FF, e[1]))
        out = [struct.pack("<BBHI", ord("F"), 1, len(entries), (now_us // 1000) & 0xFFFFFFFF)]
        for id_field, page, dlc, count, last_ms, data in entries:
            out.append(struct.pack("<HBBII8s", id_field, page, dlc, count, last_ms, data))
        return b"".join(out)

    def status_message(self, now_us: int, clients: int) -> str:
        self.fps, self.fps_count = self.fps_count, 0
        return json.dumps({
            "type": "status", "uptime_ms": now_us // 1000, "fps": self.fps, "frames": self.frames_total,
            "rx_dropped": 0, "bus_errors": 0, "bus_errors_per_s": 0, "rec": self.rec, "session": self.session,
            "sd_dropped": 0, "buffers_free": 7, "buffers": 8, "marks": self.marks, "clients": clients,
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
        t_s = self.now_us() / 1e6
        print(f"{t_s:9.1f} s  command: {text}", flush=True)


async def index(request: web.Request) -> web.Response:
    return web.Response(text=PAGE.read_text(), content_type="text/html")


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse()
    await ws.prepare(request)
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
        except ConnectionError:
            app["clients"].discard(ws)


async def start_pusher(app: web.Application) -> None:
    app["task"] = asyncio.create_task(pusher(app))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--start", type=float, default=0, help="seconds into the capture")
    parser.add_argument("--speed", type=float, default=1)
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    app = web.Application()
    app["replay"] = Replay(args.session, args.start, args.speed)
    app["clients"] = set()
    app["state"] = {"joined": False}
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.on_startup.append(start_pusher)
    web.run_app(app, port=args.port)


if __name__ == "__main__":
    main()
