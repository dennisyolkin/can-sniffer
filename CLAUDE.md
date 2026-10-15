# CAN sniffer — Citroën ë-C4 (2022)

Standalone CAN bus logger built on a LilyGO T-CAN485 (ESP32) plugged into the car's OBD2 port.

## Goals

- **Primary:** learn how CAN works, hands-on.
- **Later:** get diagnostics the car doesn't normally show (traction battery health, cell
  voltages, temperatures, SoH), and look for hidden features/settings.

### Phase 1 (current): passive logger + live view

- The board runs a Wi-Fi access point and serves a page that shows decoded car data on the phone in real time
  (design and protocol: [docs/live-view.md](docs/live-view.md)).
- Recording **every** frame to the SD card for offline analysis is a switch on that page, **off at every boot**.
  Each on/off cycle is its own session. A "Mark" button writes timestamped labels into the session.
- **Listen-only mode.** The node must never transmit or ACK anything.
- Runs **without a laptop**: powered from OBD2, no user interaction needed. If the web UI can't start, it records
  from boot instead.
- **Silent console.** No `printf`/logging in the normal data path. Status goes through the RGB LED and the page.
- The board is unplugged when the car is not in use, so power draw while parked is not a concern.

### Later phases (not started)

- Active diagnostics (UDS over ISO-TP) to query ECUs such as the BMS. This **requires leaving
  listen-only mode** and sending frames to a running car. Do it only when the user explicitly
  asks, and only for read-only services (e.g. `0x22 ReadDataByIdentifier`). Never send write,
  routine control, security access or ECU reset requests unless the user explicitly asks for that.
- Stellantis cars from roughly 2020 on may have a Secure Gateway behind the OBD port. That can
  limit what is visible passively and what active requests are allowed. Verify on the real car;
  don't assume.

## Hard rules

- **Language and framework:** plain **C**, **ESP-IDF v6.1**. No Arduino, no C++.
- **Never disable `enable_listen_only` and never call `twai_node_transmit`** unless the user
  explicitly asks. This is a real car on the road.
- Don't add console output to the hot path (ISR, RX, SD writing). Logging during init or errors is
  acceptable during development. It must be removable via sdkconfig (log level), not by
  editing code.
- Keep the RX ISR minimal: copy the frame, hand it to a task, return. No allocation, no
  logging, no blocking.
- Never call `ESP_ERROR_CHECK` on errors that can happen at runtime (SD full or removed, LED
  busy). An abort-and-reboot loop in the car is worse than a red LED.

## Hardware

Board: **LilyGO T-CAN485** (ESP32-WROOM, 2 MB flash per sdkconfig, no PSRAM). CAN transceiver
SN65HVD231 (3.3 V).

| Function        | GPIO | Notes |
|-----------------|------|-------|
| CAN TX          | 27   | Held HIGH (recessive) as a plain GPIO; **not** routed to TWAI (`io_cfg.tx = -1`). |
| CAN RX          | 26   | |
| CAN SE / Rs     | 23   | Transceiver slope control. Driven **LOW** for high-speed mode (as in LilyGO's examples). |
| SD MISO         | 2    | Strapping pin. The SD card can interfere with flashing. |
| SD MOSI         | 15   | Strapping pin (it can silence the boot log). |
| SD SCLK         | 14   | |
| SD CS           | 13   | |
| WS2812 RGB LED  | 4    | Driven via RMT (`espressif/led_strip`), GRB order. |
| 5 V boost enable| 16   | Only needed when powering external 5 V devices. Unused. |
| RS485           | 21/22/17/19 | Unused. |

SD card: SPI mode (`SPI2_HOST`), FAT, mounted at `/sdcard`.

### CAN front end, from the official schematic

Source: `project/T-CAN485.pdf` in github.com/Xinyuan-LilyGO/T-CAN485 (revision dated 2026-01-23;
older boards may differ).

- **Fixed 120 Ω termination (RY2) between CAN-H and CAN-L.** No jumper (measured about 110 Ω).
  On the car's bus, which already has 2 × 120 Ω, it adds a third terminator (about 40 Ω total).
  **On the user's board RY2 stays fitted** (an SMD part marked "121", too small to desolder
  by hand). Accepted for listen-only use. If removal is needed later: crush it with flush cutters
  or use hot air.
- The transceiver runs from **3.3 V** (VCC via 0 Ω "RF" to VDD3V3; the +5 V option "RD" is not
  fitted), so GPIO 16 / the 5 V boost is not needed for CAN.
- Rs (pin 8) connects to GPIO 23 through 4.7 kΩ. LOW = high-speed mode. There is no pull resistor,
  so the pin floats until the firmware drives it.
- TX: GPIO 27 → 2 kΩ → transceiver D.
- RX: transceiver R → 1N4148 diode → GPIO 26, with a 4.7 kΩ pull-up to 3.3 V. The transceiver can
  only pull RX low, and the low level is about one diode drop (~0.6–0.7 V) above GND. That is
  under the ESP32's V_IL max (~0.8 V), but without much margin.
- The CAN connector's ground is **AGND**, tied to board GND only through 1 MΩ ∥ 1 nF. Ground
  reference to the car comes through the power input, so the board's power ground must be the
  car's ground (OBD pins 4/5).
- CAN-H and CAN-L have a common-mode choke, polyfuses and TVS diodes.

### Car side

- OBD2 pins: 6 = CAN-H, 14 = CAN-L, 4/5 = GND, 16 = +12 V (always on).
- The OBD port diagnostic CAN is expected at **500 kbit/s**, 11-bit IDs. On PSA/Stellantis it is
  likely gatewayed, so passive traffic may be sparse. Other buses (e.g. comfort CAN at 125 kbit/s)
  may or may not appear on other OBD pins. Treat all of this as hypotheses to confirm from the
  captures.
- Pin 16 is always powered. The board will drain the 12 V auxiliary battery if left plugged in
  for days (the user unplugs it). Power can also disappear at any moment, so the log format and flushing strategy must
  handle sudden power loss.

## Architecture

```
TWAI ISR (on_rx_done) ──► ring of rx_slot_t + counting semaphores ──► logger loop (app_main, core 0)
                                                                       ├──► live_data table (latest frame per ID) ◄── web_server (core 1) ──► phone
                                                                       │      ▲ recording requests, marks ───────────────┘
                                                                       │  while recording: formats text into 8 KB RAM chunks
                                                                       ├──► free chunk queue / writer queue ──► sd_writer task (core 1) ──► SD (FAT)
                                                                       └──► status_led (state + activity blink)
```

| File | Role |
|------|------|
| [main/main.c](main/main.c) | `app_main`, TWAI setup (listen-only, 500 kbit/s), ISR to task handoff via a slot ring, the logger loop: live table per frame, SD while recording, once per second requests, marks, events, status and LED. |
| [main/sd_logger.c](main/sd_logger.c) / [.h](main/sd_logger.h) | SD mount over SPI, a fixed pool of 8 RAM chunks, sessions on demand (`sd_logger_start`/`_stop`) and the `sd_writer` task: the only code that touches the card after init. Session open/close travel through the writer queue with the data, so they stay in order. Log calls never block. |
| [main/live_data.c](main/live_data.c) / [.h](main/live_data.h) | State shared between the logger loop and the web server: latest frame per ID (per page for muxed `334`/`3F4`), status snapshot, recording request, mark queue. Short spinlock sections only. |
| [main/wifi_ap.c](main/wifi_ap.c) / [.h](main/wifi_ap.h) | NVS, netif, SoftAP (menuconfig → Live view Wi-Fi; password in `sdkconfig.secrets`), mDNS `can-sniffer.local`. |
| [main/web_server.c](main/web_server.c) / [.h](main/web_server.h) | httpd on core 1: `GET /` serves [main/web/index.html](main/web/index.html) (gzipped at build time), `/ws` pushes the frame table (binary, 5 Hz) and status (JSON, 1 Hz) and takes `rec on`/`rec off`/`mark <label>`. |
| [main/web/index.html](main/web/index.html) | The page: no frameworks. Its signal table (JSON, PSA-RE bit notation) is the decoding for the live view; check it with `tools/check_page_signals.py`. |
| [main/status_led.c](main/status_led.c) / [.h](main/status_led.h) | WS2812 status LED: `set`, timed `show_for` with a FreeRTOS one-shot timer, a "priority" flag that blocks other updates. |

TWAI uses the **new v6 driver** (`esp_driver_twai`: `esp_twai.h`, `esp_twai_onchip.h`,
`twai_new_node_onchip`, `twai_node_receive_from_isr`). Don't use the legacy
`driver/twai.h` API. Useful callbacks not used yet: `on_state_change` (bus-off and error-passive
states) and `on_error`. `twai_node_get_info` returns counters.

**ESP32 listen-only quirk:** the driver sets REC to 128 when starting in listen-only mode, as an
errata workaround so the chip can't send dominant error frames (`twai_hal_v1.c`). The node is
therefore **always `error_passive`** and REC/TEC are frozen. This is expected, not a fault. Judge
bus health by `on_error` counts and received frames, never by the error state.

### Log format

Use **candump log format** (`(seconds.micros) can0 123#DEADBEEF`, one frame per line), so the
captures open directly in can-utils, SavvyCAN, python-can and similar tools. Timestamps come from
`esp_timer_get_time()` (µs since boot). There is no RTC, so sessions are ordered by directory
only. Record the frame's own fields: ID, extended flag, RTR flag, DLC and data.

Each recording (REC switched on) creates `/sdcard/can-sniff/<boot id>-<nn>/`: the boot id is random hex per boot,
`nn` counts the sessions of that boot. It contains:
- `can.log`: every frame, candump format. Timestamps come from the TWAI driver
  (`timestamp_resolution_hz = 1 MHz`, captured in the ISR).
- `events.csv` (`timestamp_us,event,value`): logger/bus status such as `bitrate`, `bus_state`,
  `rx_dropped_total` (RX ring full; written only when it changes), `sd_dropped_total` (all write
  chunks waiting for the card; only when it changes) and `sd_stats` every second
  (`write_max_us=… sync_max_us=… buffers_min_free=n/total`, maxima and minimum over that second).
  Counters (`*_dropped_total`, `bus_errors`) start from zero in each session. `mark` lines (`<n> <label>`) come
  from the page's Mark button, stamped with the time of the press, so they can be up to a second out of order.

Throughput budget: 500 kbit/s saturated is about 4,000 frames/s, or roughly 150 KB/s of text. The
car's bus runs about 1,550 frames/s (~60 KB/s). Average bandwidth is not the problem; card latency is.
SD cards sometimes block a write or fsync for over a second (trip1: median 830 ms per second, max
1.9 s, while SD writes still ran in the RX loop, which lost 83% of frames). So the card is written
only from `sd_writer`, which works through a fixed pool of 8 × 8 KB chunks (trip3, after the MISO fix, never had
more than 4 in use; slowest write 89 ms) and fsyncs about once per second (postponed while a backlog is draining, at most 5 s). Never write
to the card from the RX path, and don't fsync per frame. Count dropped frames (`rx_`/`sd_dropped_total`),
so we know whether a capture is complete.

### Status LED

The LED is the UI when no phone is connected. Keep the meaning of each color documented here when it changes.
The logger loop blinks once per second (50 ms); the color is the state. A missing SD card is not fatal: the live
view works, and the page shows "No SD".

| LED | Meaning |
|-----|---------|
| Solid red | Fatal init error (TWAI init failed). Device halts; no reboot loop. |
| Red blink | Recording failed (card full or removed). Switch REC off and on to retry. |
| Yellow blink | Bus errors (form/stuff/...) in the last second: wiring or bitrate problem. |
| Green blink | Frames received, recording. |
| Cyan blink | Frames received, not recording. |
| Blue blink | No frames in the last second. |

## Build, flash and monitor

ESP-IDF lives at `~/.espressif/v6.1/esp-idf`, installed with EIM. Target `esp32`.

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh   # not esp-idf/export.sh (no python_env there)
idf.py -B build build
idf.py -B build -p /dev/tty.usbserial-595D0130391 flash monitor   # Ctrl-] to exit monitor
idf.py -B build menuconfig
```

- Pass `-B build`: `CMakePresets.json` (used by the VS Code extension) otherwise makes `idf.py` configure into a
  broken `${sourceDir}/build/default` directory.
- `sdkconfig` is git-ignored. Any config that matters goes in **`sdkconfig.defaults`** (flash size and partition
  table, tick rate, WebSocket support, Wi-Fi/lwIP on core 1). Defaults only apply to options not yet in
  `sdkconfig`: after changing it, `rm sdkconfig` and rebuild.
- Wi-Fi credentials: `sdkconfig.secrets` (git-ignored, template `sdkconfig.secrets.example`), added as a second
  defaults file by the top-level `CMakeLists.txt`.
- [partitions.csv](partitions.csv): one ~1.9 MB factory app (the app is ~1 MB with Wi-Fi; the 1 MB default doesn't
  fit). No OTA.
- The phone joins the `can-sniffer` network and opens `http://192.168.4.1/` (or `http://can-sniffer.local/`).
- `CONFIG_FREERTOS_HZ=100`, so one tick is 10 ms. Never add `vTaskDelay` to the RX path;
  `vTaskDelay(1)` alone would cap throughput at about 100 frames/s.
- Dependencies come from the component manager (`main/idf_component.yml`, `led_strip ^3`).
  `managed_components/` is generated.
- clangd uses `build/compile_commands.json`. Build once before relying on diagnostics.
- There are no host tests. Verification is on hardware. When changing the TWAI or SD path,
  describe what to observe (LED, files on the card) rather than claiming it works.
- The page can be developed without the car: `tools/live_replay.py` serves it and replays a capture over the
  same WebSocket protocol (`uv run --project tools tools/live_replay.py ~/tmp/trip3 --start 150`, then
  http://localhost:8080/). The page is re-read on every request.

## Code conventions

- C11, 4-space indent, max 120 columns, `snake_case`. Module prefix on public API
  (`status_led_*`, `sd_logger_*`). `static` for all file-local functions and state.
- This deliberately differs from the Espressif style guide, by the user's choice:
  - **No `s_` / `g_` prefixes** on static or global variables: `static bool initialized;`.
  - **Opening brace on the same line everywhere**, including function definitions:
    `void foo(void) {`.
  - Don't run IDF's `tools/format.sh` (astyle); it would rewrite the braces.
- Never start identifiers with `_` (reserved in C).
- Public functions return `esp_err_t`. Propagate errors; don't swallow them.
- Pin and size constants are `#define`s at the top of the owning `.c` file.
- One module per concern (`<name>.c` + `<name>.h`), registered in
  [main/CMakeLists.txt](main/CMakeLists.txt) `SRCS`, with its IDF components in `REQUIRES`.
- Comments explain *why* (hardware quirks, timing constraints), not *what*.

## Analysis (off-device)

Captures are analyzed on the laptop. If helper scripts are added, put them under `tools/`
(Python is fine there), separate from the firmware.

- Run with `uv run --project tools tools/<script>.py <session_dir> ...` (deps in `tools/pyproject.toml`).
  `canlog.py` parses `can.log` and caches it as `can.parquet` in the session directory.
- `check_page_signals.py` decodes a capture with the live page's signal table (first/last/min/max per signal);
  run it after changing that table. `live_replay.py` replays a capture to the page (see Build).
- `inventory.py` (health + per-ID table), `bytestats.py` (per-byte classes), `plot.py` (fields over time,
  to PNG), `correlate.py` (rank fields against a reference), `trends.py` (slowly rising/falling fields).
- `psare.py` decodes a capture with [PSA-RE](https://github.com/prototux/PSA-RE) frame definitions (needs a local
  checkout, kept at `~/projects/repos/PSA-RE`; the ë-C4 is `AEE2010.electric`, bus `HS.HS1`). PSA-RE covers 79 of the 87 IDs on this bus,
  but not every definition fits this car; check values before trusting one.
- Decoded signals, confidence levels and open questions: [docs/signals.md](docs/signals.md).
