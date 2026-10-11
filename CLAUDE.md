# CAN sniffer — Citroën ë-C4 (2022)

Standalone CAN bus logger built on a LilyGO T-CAN485 (ESP32) plugged into the car's OBD2 port.

## Goals

- **Primary:** learn how CAN works, hands-on.
- **Later:** get diagnostics the car doesn't normally show (traction battery health, cell
  voltages, temperatures, SoH), and look for hidden features/settings.

### Phase 1 (current): passive logger

- Dump **every** frame on the bus to the SD card for offline analysis on the laptop.
- **Listen-only mode.** The node must never transmit or ACK anything.
- Runs **without a laptop**: powered from OBD2, starts logging on boot, no user interaction.
- **Silent console.** No `printf`/logging in the normal data path. Status goes through the RGB LED.

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
  for days. Power can also disappear at any moment, so the log format and flushing strategy must
  handle sudden power loss.

## Architecture

```
TWAI ISR (on_rx_done) ──► ring of rx_slot_t + counting semaphores ──► logger task ──► buffered writes ──► SD (FAT)
                                                                       │
                                                                       └──► status_led (state + activity blink)
```

| File | Role |
|------|------|
| [main/main.c](main/main.c) | `app_main`, TWAI setup (listen-only, 500 kbit/s), ISR to task handoff via a slot ring. |
| [main/sd_logger.c](main/sd_logger.c) / [.h](main/sd_logger.h) | SD mount over SPI, a session directory per boot (`/sdcard/can-sniff/<random hex>/`), file writing, flush/fsync. |
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

Each boot creates `/sdcard/can-sniff/<random hex>/` with:
- `can.log`: every frame, candump format. Timestamps come from the TWAI driver
  (`timestamp_resolution_hz = 1 MHz`, captured in the ISR).
- `events.csv` (`timestamp_us,event,value`): logger/bus status such as `bitrate`,
  `rx_dropped_total` (written only when it changes) and `bus_state`.

Throughput budget: 500 kbit/s saturated is about 4,000 frames/s, or roughly 150 KB/s of text.
That's fine for SD over SPI if writes are batched (large `setvbuf` buffer, periodic `fflush` +
`fsync` every ~1 s). Don't fsync per frame. Count dropped frames (ring full) and log the count
periodically, so we know whether a capture is complete.

### Status LED

The LED is the only UI. Keep the meaning of each color documented here when it changes. While
running, the logger loop flushes to SD once per second and blinks once (50 ms); the color is the
state.

| LED | Meaning |
|-----|---------|
| Solid red | Fatal init error (SD missing or mount failed, TWAI init failed). Device halts; no reboot loop. |
| Red blink | SD write or flush failed (card full or removed). Logging has stopped. |
| Yellow blink | Bus errors (form/stuff/...) in the last second: wiring or bitrate problem. |
| Green blink | Frames received in the last second. |
| Blue blink | Running, no frames in the last second. |

## Build, flash and monitor

ESP-IDF lives at `~/.espressif/v6.1/esp-idf`, installed with EIM. Target `esp32`.

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh   # not esp-idf/export.sh (no python_env there)
idf.py build
idf.py -p /dev/tty.usbserial-595D0130391 flash monitor   # Ctrl-] to exit monitor
idf.py menuconfig
```

- `sdkconfig` is git-ignored. Any config that matters (log level, FATFS options, tick rate) goes
  in **`sdkconfig.defaults`**, which doesn't exist yet. Create it when the first such setting is
  needed.
- `CONFIG_FREERTOS_HZ=100`, so one tick is 10 ms. Never add `vTaskDelay` to the RX path;
  `vTaskDelay(1)` alone would cap throughput at about 100 frames/s.
- Dependencies come from the component manager (`main/idf_component.yml`, `led_strip ^3`).
  `managed_components/` is generated.
- clangd uses `build/compile_commands.json`. Build once before relying on diagnostics.
- There are no host tests. Verification is on hardware. When changing the TWAI or SD path,
  describe what to observe (LED, files on the card) rather than claiming it works.

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
