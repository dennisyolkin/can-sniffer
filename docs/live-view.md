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
- **Text, every second** (and at once when a page connects): JSON status, `{"type":"status", …}` with `uptime_ms`,
  `fps`, `frames`, `rx_dropped`, `bus_errors`, `bus_errors_per_s`, `rec` (`no_card`/`off`/`on`/`failed`),
  `session`, `sd_dropped`, `buffers_free`, `buffers`, `marks`, `can_mode` (`listen_only`/`tx`; drives the
  TX badge), `bus_state` (TWAI error state, only meaningful in `tx` mode), `clients`, `heap_free`,
  `heap_min`.
- **Text, TX mode only:** `{"type":"uds","requests":[…],"log":[…]}` when the request log changes and when a page
  connects (see "Transmitting" below).

Page → server (text): `rec on`, `rec off`, `mark <label>` (label up to 31 chars; commas, quotes and control
characters are replaced), and in TX mode `uds <name>` (ignored while a request is in flight). Longer than 64
bytes closes the connection.

Muxed IDs (keyed by page so all pages are kept): `334` (cell voltages, page = b0 >> 3) and `3F4` (temperatures,
page = b0 >> 5).

## Page

Top bar, always visible: connection state, frames/s, **REC switch**, **Mark** button (presets + text), uptime.

| Tab | Content |
|-----|---------|
| Drive | Speed, SoC, power estimate (−dAh/dt × V from `563` and `5C3`) with a 60 s sparkline, **This trip** (energy regenerated / drawn / net from the `3A8` counters with % recovered, the same from `563` × `5C3` as a check, km and Wh/km; kept in localStorage until Reset), gear / mode / ready, accelerator, brake pressure, torque, steering angle, camera speed limit, odometer. |
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

## Transmitting (test builds only)

Leaving listen-only makes the node ACK every frame, so it is a bus participant even when it sends nothing. That is
a build option, not a runtime switch:

- Listen-only (default, the car image): the node sends nothing, not even ACKs.
- `CONFIG_CAN_TX_MODE` (build `build-tx`): normal TWAI mode, the node ACKs every frame, and `uds_client` adds a
  "Diagnostics" block to the Raw tab with one button per predefined request; there is no free-form send box.
  Header badge "TX".

History: an ACK-only build (normal mode, nothing sent) was tested in the car on 2026-10-20: no warnings, no bus
errors, ~1.5k frames/s as usual. It was later merged into TX mode.

Requests (`uds_client.c`, allowlisted in C to UDS `0x3E`, `0x22`, `0x19` (`01`/`02`/`04`/`06`), `0x10`
(`01`/`03` only, red buttons) and OBD-II mode `01`, single frames only; the services that change the ECU are
documented, not implemented, in [uds-write-services.md](uds-write-services.md)):

| Button | Frame | Reply |
|--------|-------|-------|
| VCU: TesterPresent | `6A2#023E000000000000` | `682#027E00` |
| BMS: TesterPresent | `6B4#023E000000000000` | `694#027E00` |
| ABS: TesterPresent | `6AD#023E000000000000` | `68D#027E00` |
| BMS: 22 reads (below) | `6B4#0322xxxx00000000` | `694#0n62xxxx` + value, or `694#037F22nn` |
| BMS: 5 long reads (below) | `6B4#0322xxxx00000000` | ISO-TP multi-frame reply |
| BMS extra: 30 more reads (below) | `6B4#0322xxxx00000000` | single frame |
| BMS faults: Count | `6B4#03190109` | `59 01 <avail> <format> <count hi> <count lo>` |
| BMS faults: Active / confirmed | `6B4#03190209` (status mask 09) | `59 02 <avail>` + 4 bytes per fault |
| BMS faults: History | `6B4#0319022D` (mask 2D: also pending and "failed since last clear") | as above |
| BMS faults: Snapshot / Extended data (from a fault's row) | `6B4#061904 <DTC> FF` / `061906 <DTC> FF` | `59 04` / `59 06` + records |
| BMS session: Default / Extended (red) | `6B4#021001` / `021003` | `50 01` / `50 03` + P2 / P2* timing |

BMS reads (`0x22`, one DID per request; PSA-RE `TBMU/BMU_CTE1`, value ≤ 4 bytes so the reply is one frame). The
page decodes them (`DIDS` in `index.html`: big-endian value at byte offset `o`, `raw × f + off`), keeps the newest
value per DID, and "Read all" sends them one after another (next one when the previous is done, or after 3 s).
Checked against the broadcast frames in diag-bms-1: every request answered, and the values that have a broadcast
twin match it (details and status per DID in docs/signals.md, "Traction battery over UDS"):

| DID | Value | Encoding |
|-----|-------|----------|
| D810 | SoC | 2 B × 1/512 % |
| D860 | SoH ("capacity to supply and store energy") | bytes 1–2 of 3, × 1/16 % |
| D413 | Cells minimum capacity | 2 B × 1/512 % |
| D865 | Energy | 2 B × 1/64 kWh |
| D815 | Pack voltage | 2 B × 1/16 V |
| D816 | Current | 4 B × 1/64 − 1200 A |
| D43D / D86F / D870 | Average / lowest / highest cell voltage | 2 B × 0.001 V |
| D43C / D43B | Number of the lowest / highest cell | 1 B, 0-based (page shows +1) |
| D877 / D87D / D817 | Average / min / max temperature | 1 B − 40 °C |
| D878 | Temperature spread | 1 B °C |
| D871 / D873 | Max charge / discharge power for 10 s | 4 B × 1/128 − 500 kW |
| D811 | Battery status | enum: normal, downgraded, … |
| D86D | Cell balancing status | enum: correct, incorrect |
| D47A | Insulation resistance | 4 B kΩ |
| D822 | 12 V battery voltage as the BMS sees it | 2 B × 0.001 V |
| F18B | ECU manufacturing date | 3 B BCD |

Long BMS reads (multi-frame replies, reassembled with ISO-TP; all five answered in bms-isotp-1, values in
docs/signals.md):

| DID | Value | Page shows |
|-----|-------|------------|
| D4B5 | `DATA_CELL_SOH`: 108 cells × 2 B | min / avg / max and the 3 weakest cells, ÷16 % like D860 (fits: 91.9-93.8 %) |
| F080 | ECU identification: part number (BCD), supplier (`0074` = CATL), additional reference | decoded |
| F0FE | Supplier identification: software edition, download date, number of downloads, software reference | decoded |
| D901 | BMS serial number, ASCII | decoded |
| D465 | 423 B here (PSA-RE: 408), unnamed; SoH histories and counters (docs/signals.md) | BMS active time (b148-150, seconds, probably) + hex |

BMS extra (PSA-RE encodings, not checked against the car yet): min / max charge level `D862`/`D863` (×1/512 %),
SoC sensor calibration `D410`, why the switches last opened `D812`, switch state `D813`, switch welded `D814`,
fuse F1 `D86B`, insulation + / − `D87B`/`D87C` (kΩ), 30 s power limits `D872`/`D874` and 10 s current limit
`D876`, hottest / coldest module `D445`/`D446`, mileage `D49C` (×1/1024 km) and time counter `D4CA` (×0.1) as the
BMS sees them, CAN fault record `D89E`, and the unnamed `F18C D900 E082 D416-D419 D41E-D423` (shown raw).

Fault codes: each is 2 code bytes + a failure type byte + a status byte (ISO 14229). The page names them from
PSA-RE's BMS list (embedded in `index.html` by `tools/bms_dtc_names.py`), explains the status bits (failing now,
failed this drive, pending, confirmed, failed since last clear, warning lamp), and gives each fault Snapshot /
Extended data buttons. The page parameter (the 3-byte DTC) is placed at a fixed position by `uds_client` and the
finished frame is checked against the allowlist. The session buttons are red: `10 03` changes how the BMS
behaves until it falls back to the default session (~5 s after the last request, unless TesterPresent keeps it).

ISO-TP: a reply longer than 7 bytes starts with a first frame (`1L LL` + 6 bytes, `LLL` = total length). The node
answers on the request ID with a flow control `30 00 01`: continue, block size 0 (send everything), at least 1 ms
between frames. The ECU then sends consecutive frames (`2N` + 7 bytes, `N` counting 1..15, 0, …). The node only
sends a flow control in answer to a first frame from the ECU it just asked. Replies up to 512 bytes are followed
(`UDS_CLIENT_MSG_MAX`); a longer one gets no flow control, and the ECU gives up by itself. A missing or
out-of-order frame, or a gap of more than 1 s, abandons the reply. The page log shows the first frame, our flow
control and "reply complete: N bytes" (not each consecutive frame; they are all in `can.log`); the values panel
decodes the known DIDs, and each long reply has an expandable hex dump.

Tried from the OBD port and removed because nothing ever answered (addresses from PSA-RE; kept here in case another
port, session or address makes them work):

| Request | Frame | Expected reply |
|---------|-------|----------------|
| BSI: TesterPresent | `752#023E000000000000` | `652#027E00` |
| BSI: read DID 010A | `752#0322010A00000000` | `652#0462010Axx` (1-byte "BSI has gone dormant" flag) |
| OBD-II: supported PIDs | `7DF#0201000000000000` | `7E8`–`7EF`: `064100…` from each ECU that answers |
| Cluster: TesterPresent | `75F#023E000000000000` | `65F#027E00` |

First car test (2026-10-21, `tx-logs-1`): every BSI and OBD-II request was ACKed, but nothing answered. The direct
TesterPresent requests to ECUs on HS1 were added to tell "BSI unreachable from this port" apart from "diagnostics
blocked".

Second test (2026-10-22, `tx-logs-2`): **VCU, BMS and ABS answer** TesterPresent (`02 7E 00`, 3-byte frames without
padding; VCU ~10 ms, BMS 10-21 ms, ABS 1-6 ms). The cluster, BSI and OBD-II stay silent. So pins 6/14 are on HS1
itself, and the ECUs there take diagnostic requests directly, with no gateway in between.

One request at a time. The node must be `error_active`, or the request is refused. A request is single-shot on the
ESP32, so it is retried up to 5 times, 5 ms apart, if it loses arbitration or hits a bus error (back-to-back
retries all landed in the same burst of car frames). Replies are collected for 1 s (a physical request stops at its
first final reply; "response pending" keeps waiting; each consecutive frame of a multi-frame reply restarts the
second). The node receives its own requests back (loopback), so they appear in `can.log`; `events.csv` gets `uds_tx`, `uds_rx`, `uds_fc` (our flow control), `uds_msg` (`<request>
<hex>`, a complete multi-frame reply), `uds_rx_error` (`<request> <sequence|too_long|incomplete> <received>/<len>`),
`uds_done`, `uds_tx_failed` and `uds_refused`.

The WebSocket gets a `{"type":"uds","requests":[…],"log":[…]}` message whenever the log changes, and once when a
page connects; the page sends `uds <name>` or `uds <name> <hex>` (the parameter of a request that takes one).
`requests` is `{name, label, param, danger}` per request (`param` = parameter bytes, 0 for none). `log` holds the last 16
entries, oldest first: `{t, kind, req, id, len, data}` with `t` in board ms, `id` as a decimal number, `data` as 8
bytes of hex, and `kind` one of `tx`, `rx`, `done`, `tx_failed`, `refused`, `message`, `rx_error` (for `done` and
`tx_failed`, `len` is the number of replies or attempts; for `message` the reply length; for `rx_error` 0 =
sequence, 1 = too long, 2 = incomplete). After the first multi-frame reply the message also carries
`"msg":{"seq","t","req","id","len","data"}`: the latest complete reply, UDS payload as hex.
