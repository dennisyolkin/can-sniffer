# Live view on the phone (phase 1b)

Goal: the board runs a Wi-Fi access point and serves a small web page that shows decoded car data in real time.
Recording the full log to SD becomes optional: a switch on the page, **off at every boot**. Still passive:
listen-only, nothing is sent on the bus.

## Decisions

- **Transport:** SoftAP + `esp_http_server` + one WebSocket (`/ws`). Ported from `~/playground/esp32-softap`.
  The page is one `main/web/index.html` (no frameworks), gzipped at build time and served from flash.
- **Raw frames to the phone, decoding in JS.** The firmware keeps the latest payload of every ID (muxed IDs
  per page) and pushes the whole table about 5 times per second as one binary message. The page holds the signal
  table in PSA-RE bit notation and decodes it. Reasons: signal fixes don't touch C code, the same table can be
  checked against a capture in Python (`tools/check_page_signals.py`), and a live raw view comes for free.
- **Recording switch.** Boot mounts the card (if present) but creates no session. "Rec on" opens a new session
  directory and "rec off" closes it, so one boot can produce several sessions: `/sdcard/can-sniff/<boot hex>-<nn>/`
  (`nn` counts up within a boot). A missing card is no longer fatal; the page shows "no SD card" and the live
  view keeps working. If the web server can't start, recording turns on by itself so the board is still useful.
- **Marks.** A button on the page writes `mark` events to `events.csv` with the time of the press, numbered within
  the session (`3 A/C`), with preset labels and free text. This replaces rebuilding action logs from memory.
- **Memory.** trip3 never had more than 4 of 24 write chunks in use (89 ms worst write). The pool becomes a fixed
  8 × 8 KB (64 KB, ~1 s of traffic at the car's rate) allocated at boot, which frees ~128 KB for Wi-Fi.
- **Cores.** Core 0: logger loop and TWAI ISR. Core 1: Wi-Fi, lwIP, httpd and `sd_writer`. The RX ring (256
  frames, ~165 ms) covers the rest.
- **Flash.** Custom partition table: one ~1.9 MB factory app (the 1 MB default is too small with Wi-Fi). No OTA.
- **Power** is not optimised: the board is unplugged when the car is not in use.

## Firmware modules

| File | Role |
|------|------|
| `main.c` | TWAI, RX ring, logger loop. Per frame: live table update, then SD if recording. Each second: recording requests, marks, events, status for the page, LED. |
| `sd_logger.c` | Mount + fixed chunk pool + `sd_writer` at boot. `sd_logger_start` / `sd_logger_stop` open and close sessions; the writer does the card work, in order with the data. |
| `live_data.c` | State shared between the logger loop and the web server: latest frame per ID/page, status snapshot, recording request, mark queue. Short spinlock sections, no blocking. |
| `wifi_ap.c` | NVS, netif, SoftAP (WPA2, credentials in `sdkconfig.secrets`), mDNS `can-sniffer.local`. |
| `web_server.c` | httpd, `GET /` (gzipped page), `/ws`: binary frame table at 5 Hz, JSON status at 1 Hz; text commands from the page. |

### WebSocket protocol

Server → page:
- **Binary, every 200 ms:** header `u8 'F', u8 version=1, u16 count, u32 now_ms`, then `count` entries of 20 bytes
  (little-endian): `u16 id` (bit 15 = RTR), `u8 page` (0xFF = not muxed), `u8 dlc`, `u32 count`, `u32 last_ms`,
  `u8 data[8]`.
- **Text, every second:** JSON status: uptime, frames/s, totals, drops, bus errors, recorder state and session
  name, SD buffers, mark count.

Page → server (text): `rec on`, `rec off`, `mark <label>` (label up to 31 chars; commas and control characters
are replaced).

Muxed IDs (keyed by page so all pages are kept): `334` (cell voltages, page = b0 >> 3) and `3F4` (temperatures,
page = b0 >> 5).

## Page

Top bar, always visible: connection state, frames/s, **REC switch**, **Mark** button (presets + text), uptime.

| Tab | Content |
|-----|---------|
| Drive | Speed, SoC, power estimate (−dAh/dt × V from `563` and `5C3`) with a 60 s sparkline, gear / mode / ready, accelerator, brake pressure, torque, steering angle, camera speed limit, odometer. |
| Battery | SoH capacity / resistance, voltage, remaining Ah, min / max cell and spread, 108-cell bar chart, 54 temperatures as a grid, insulation resistance. |
| Car | Temperatures (outside, inverter, stator, DC/DC, evaporator, battery average), 12 V and DC/DC setpoint, doors (top-view SVG), locks, belts, lights, parking brake. |
| Raw | Every ID: period, count, bytes; changed bytes flash. Plus logger health (drops, SD buffers, bus errors). |

Each value carries its confidence from `docs/signals.md` (confirmed / PSA-RE / doubtful). The power estimate is
ours: in trip1 it gives 3.9 kWh for the drive (median 6.5 kW, regen down to −17 kW). The `3A8` energy counters
gave nonsense there, so they are not used.

## Status LED

See CLAUDE.md, "Status LED": green = recording, cyan = live only, blue = no frames, yellow = bus errors,
red blink = recording failed, solid red = TWAI init failed.

## Status

Implemented (2026-10-14), not yet tried on the car. Checked on the laptop: the firmware builds (app ~1 MB of the
1.9 MB partition, page 11.5 KB gzipped); `tools/check_page_signals.py` gives the expected ranges on trip1 and
trip3; `tools/live_replay.py` + Chrome at phone size render all four tabs, and REC / Mark round-trip.

To check on the car:
- The phone joins `can-sniffer` and the page loads at `http://192.168.4.1/`; values move while driving.
- LED is cyan after boot; REC turns it green and creates `/sdcard/can-sniff/<boot id>-01/`, the next REC `-02`.
- `inventory.py` on a recorded drive with the phone connected reports 0 dropped frames (Wi-Fi is new load).
- Marks appear in `events.csv`; the power estimate looks sane against the dashboard (accelerating vs regen).
- Booting without an SD card: live view works, the page shows "No SD".

## Later: transmitting (not part of this)

Leaving listen-only makes the node ACK every frame, so it is a bus participant even when it sends nothing.
Plan for a runtime mode switch (re-create the TWAI node), a separate `uds_client` module compiled in only with an
sdkconfig option, an "ACTIVE ON BUS" indicator, and predefined read-only requests (`0x22`) instead of a free-form
send box.
