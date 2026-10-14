# Decoded signals — Citroën ë-C4 (2022), OBD2 pins 6/14, 500 kbit/s

Working notes from reverse engineering. Byte numbering in this file is **b0..b7** as in candump (0-based);
PSA-RE uses **1-based** bytes (`3.7-4.0` = our b2..b3). Multi-byte fields are big-endian unless noted.

Status: **confirmed** = matches a dashboard reading; **verified** = our own analysis of the capture and
PSA-RE agree; **PSA-RE** = from PSA-RE and plausible on our data, not checked independently;
**ours** = our own finding, not in PSA-RE; **doubtful** = PSA-RE definition gives odd values on this car.

Sources:
- **trip1** (2026-10-11): ~32 min drive, 23 km, from cold start to park. Only 16.8% of frames were logged
  (logger stalls), so rare frames and exact timing are unreliable in it.
- **trip2** (2026-10-12): ~3.6 min parked: wake with brake pedal, ready on, P → D, drive mode button, parking
  brake auto-release and re-apply, D → P, off, driver door open/close, lock from inside. 57.5% of frames
  logged (SD writer too slow), but every state change was caught. Action log under "Ground truth (trip2)".
- **trip3** (2026-10-14): ~5 min, **100 % of frames logged** (no drops). Bus asleep twice, ready on, R, D,
  ~12 m drive, off, every door and the boot, lock/unlock by key fob and keyless, lights from the fob.
  Action log under "Ground truth (trip3)".
- **PSA-RE** ([github.com/prototux/PSA-RE](https://github.com/prototux/PSA-RE)): community database of PSA
  frames in YAML. The ë-C4 is car code `C41`, architecture `AEE2010.electric` (child of `.ev`, `.full`).
  Decode a capture with `tools/psare.py <session> <PSA-RE checkout>`; it resolves the variant chain.

## Which bus this is

The OBD pins 6/14 carry PSA's **HS1 (powertrain) bus**: 79 of our 87 IDs are defined in PSA-RE's
`AEE2010.{electric,ev,full}/HS.HS1`, and the decoded values make sense. Not in PSA-RE:
`302 352 40B 42B 44B 494 4A2 66B` (see "IDs not in PSA-RE"). Across trip1-3 we have seen 96 IDs; some only
appear while the car sleeps, wakes or locks.

87 IDs, all 11-bit, ~1,550 frames/s, no bus errors. Periods: 10 ms (`208 262 2F5 305 349 3CD`),
20 ms (`2EB 302 30D 334 348 34D 3AD 40D`), 40 ms, 50 ms, 100 ms, ~1 s (5xx/7xx). Many frames end or start
with a 4-bit checksum + 4-bit process counter.

## Traction battery

| ID | Bytes (ours) | Signal | Decode | Status | trip1 |
|----|--------------|--------|--------|--------|-------|
| 3A8 | b4-b5[7:6] | Relative state of energy (= dashboard SoC) | ×0.125 % | confirmed | 62 → 53.1 %. Our b3 = whole %, b4 = 0.5 % steps |
| 5C3 | b2[1:0]+b3 | HV_BATTERY_SOC_2 | ×0.1 % | confirmed | 62.0 → 53.2 % |
| 5C3 | b0-b1[7:5] (11 bits) | HV battery voltage | ×0.25 V | PSA-RE | 407 → 400 V |
| 563 | b4-b6[7:4] (20 bits) | BMS remaining capacity | ×0.001 Ah | PSA-RE | 68.2 → 58.5 Ah (~3.9 kWh at 400 V for 23 km) |
| 543 | b0-b1 | BMS remaining energy | Wh | doubtful | 17.5 → 15.3 kWh; "total energy" 32.3 kWh doesn't fit a 50 kWh pack; frame is 4 bytes here, 6 in PSA-RE. Was our "second SoC" guess |
| 334 | mux b0[7:3] | **All 108 cell voltages**, 4 per page | V | PSA-RE | 3.78 → 3.71 V, spread ~10-20 mV |
| 5A3 | | Max / min cell voltage + cell number, insulation resistance | V, kΩ | PSA-RE | max 3.78 → 3.72, min 3.76 → 3.70 |
| 3F4 | mux b0[7:5] | **All 54 temperature probes**, 6-7 per page | raw − 40 °C | verified | 12-13 °C all trip |
| 583 | b2 | Min battery temperature | raw − 40 °C | PSA-RE | 12 °C. b0/b1 = probe numbers of max/min |
| 583 | b5, b6 | Probe counts: 54 temperature, 108 voltage | | verified | Not outside temperature, as I first guessed |
| 583 | b3 bit 5 | "High battery temperature alert" | | doubtful | Set for the whole trip at 12 °C; probably mislabelled |
| 522 | b4 | HV battery average temperature | raw − 40 °C | PSA-RE | 12 °C |
| 454 | | **SoH capacity 91 %, SoH resistance 91 %**, main relay counter 6200, DC-charge relay counter 800, lifetime 11,375 Ah | | PSA-RE | Constant (sent ~1/s). Check against a dealer/app SoH reading |
| 3A8 | b0, b1, b2 | Consumed / balance / regenerated energy counters | ×2 Wh | PSA-RE | Wrap around |
| 462 | | Charge mode, fast-charge stop request | enum | PSA-RE | "No load" while driving |

## Driving

| ID | Bytes (ours) | Signal | Decode | Status | trip1 |
|----|--------------|--------|--------|--------|-------|
| 30D | b0-1, b2-3, b4-5, b6-7 | Wheel speeds FL, FR, RL, RR (unfiltered) | ×0.01 km/h | verified | Max 91.7. Integrates to 23.4 km = odometer delta. Limiter 50 / 80 on the dash = 48.0 / 77.5 here |
| 44D | b0-1, b2-3, b4-5 | Front average, RL, RR wheel speeds (ABS) | ×0.01 km/h | verified | |
| 44D | b6-7 | Front wheels average | rpm | verified | Was our "1.95 × speed" |
| 38D | b0-1 | Vehicle speed from wheels | ×0.01 km/h | verified | |
| 38D | b2-3, b4 | Distance (m, wraps), longitudinal acceleration | | PSA-RE | Accel −3.8 … +1.5 m/s² |
| 38D | b6 | Direction (forward/rear), moving/stopped | enum | PSA-RE | |
| 552 | b4-6 | **Odometer** | km | confirmed | 25,650 → 25,673 |
| 552 | b0-3 | Vehicle time counter | ×0.1 s | verified | |
| 305 | b0-1 signed | Steering wheel angle | ×0.1 ° | verified | ±500 ° |
| 305 | b2 / b5 | Steering rotation speed (+ optimised) | °/s | verified | b3 bit 7 = direction |
| 2F5 | b3-4 / b5-6 / b1 | EPS column angle / column speed / driver torque | 0.1 °, °/s, Nm | verified | Driver torque ±2.5 Nm |
| 3CD | b0-1 / b2-3 | Lateral acceleration / yaw rate | m/s², °/s | verified | Our guesses, now named |
| 3CD | b5-6 | Brake master cylinder pressure | bar | PSA-RE | Up to 41 bar |
| 3CD | b6 bits 1-0 | Braking in progress | enum | PSA-RE | |
| 208 | b3 | Driver demand (accelerator) | ×0.5 % | verified | Max 58 %. Also `56E` b3 ACCELERATOR_PEDAL_POSITION |
| 208 | b2 / b5 | Actual torque / torque without gearbox reduction | Nm (−100 offset) | PSA-RE | b5 was our "motor torque" |
| 208 | b4 bit 1, `412` b0 bit 5 | Brake pedal switch | bool | PSA-RE | Pressed 12 % of the time |
| 208 | b4 bits 3-2 | Cruise / speed limiter status | enum | PSA-RE | |
| 349 b3, 489 b0, 4FE b1, 563 b3 | | Gear lever P / R / D | enum | confirmed | D 91 %, P 7-9 %, R 1 %. `4FE` b1[7:5] 0 = P, 1 = R, 3 = D (trip2/3); 2 appears for 0.1 s between P and R, probably N; `489` b0[3:0] follows ~40 ms later. `4FE` b2[6:4] = takeover request (2 = P, 5 = D) |
| 572 / 412 | b0 bit 1 / b0 bit 2 | Reverse engaged | bool | confirmed | trip3: set ~0.6 s after the lever reaches R, cleared ~0.15 s after D |
| 3AD | b0, b3 | Electric parking brake state, force | enum | confirmed | Applied 6 %. trip2: b3[2:0] 1 = applied, 3 = releasing, 0 = released, 2 = applying (auto-release on accelerator in D, re-apply with the switch) |
| 512 / 5E2 | | Drive mode (Eco 93-95 %, Normal), energy mode ZEV | enum | confirmed (`512`) | trip2: `512` b0[7:5] 0 = Normal, 2 = Eco; resets to Normal at power-off |
| 5C1 | b5 / b6 | Front motor inverter / stator temperature | raw − 40 °C | PSA-RE | 12 → 24-27 / 12 → 32-35 °C. Our "5C1 temperatures" |
| 5C1 | b7[7:3] | Motor HV voltage | V | PSA-RE | |
| 522 | b0 | DC/DC converter temperature | raw − 40 °C | PSA-RE | 16 → 33 °C. Our "522 temperature" |

## Body, 12 V, environment

| ID | Bytes (ours) | Signal | Decode | Status | trip1 |
|----|--------------|--------|--------|--------|-------|
| 5B2 | b2 | **Outside air temperature** | ×0.5 − 40 °C | PSA-RE | 12 → 14 °C; user saw 14-15 °C |
| 50D | b5 | **12 V battery voltage** (measured by brake ECU) | ×0.05 V | PSA-RE | 12.15 V at start, 11.55-14.8 V |
| 468 | b3-b4 | DC/DC 12 V voltage setpoint | V | PSA-RE | 10.6 … 14.6 V |
| 612 | b1 bits 7-6 | Turn indicator switch: left / right | enum | PSA-RE | |
| 40E / 612 | | Low / high beam, DRL | bool | PSA-RE | DRL on, low beam off (daytime) |
| 40E / 612 | `40E` b4 bit 3 / bit 1, `612` b1 bit 1 | Side lights / low beam command, low beam state | bool | confirmed (on) | trip3: key-fob lights button turns side + low beam on; they read on for exactly 30.0 s although the user turned them off sooner (see "Not found yet") |
| 40E | b2 bit 6 | DRL command | bool | ours | Off while parked. trip3: on 3 s at every lock, 6 s at every unlock (fob and keyless), ~0.3 s before `512` changes: the lock/unlock light signature |
| 572 | b0 | Seat belts (driver / passenger), reverse engaged | enum | PSA-RE | |
| 412 | b6[7:3] | Doors / boot open | bit field | confirmed | trip3: 1 = front left (driver), 2 = front right, 4 = rear left, 8 = rear right, 0x10 = boot |
| 412 | b0 bit 5 | Brake pedal switch | bool | confirmed | trip2: pressing it wakes the bus (~77 → ~1,570 frames/s) |
| 572 | b0[3:2] | Powertrain state (ready) | enum | confirmed | trip2: 0 = off, 1 = starting (~0.9 s), 2 = ready |
| 512 | b1 | Locked from inside / outside | enum | confirmed (inside) | "Super-locked" 92 % while driving? trip2: b1[4:3] = 2 after locking from inside, back to 0 on unlock. So 2 means locked from inside (PSA-RE's "super-locked" label is wrong here), which explains the 92 % during trip1 if the doors auto-lock while driving. trip3: key fob and keyless both give 2; b1[2:1] ("locked from outside") stayed 0, so the two aren't distinguishable here |
| 3CE | b1, b4-5 | Evaporator air / probe temperature, heater water | °C | PSA-RE | 12-14 °C |
| 3CE | b4[7:3], b4[2:0]+b5, b6+b7[7] | Evaporator setpoint (×0.5 °C), evaporator probe (×0.1 − 40 °C), blower airflow | | verified | trip3: 3 s after ready the setpoint goes 6.5 → 3.0 °C and the probe falls 14.4 → 0.5 °C until power-off: compressor running |
| 348 | b3 | Refrigerant pressure (PSA-RE: COOLING_SYSTEM_PRESSURE_2) | raw | ours | 0 → 48 when the evaporator setpoint drops in trip3. Scale unknown |
| 56B / 5F2 / 59E | | Wipers, camera speed-sign reading (30/50/100), traffic-sign IDs | | PSA-RE | |
| 3F2 | b4-b7 | Lane keeping assist state, requested yaw rate, column setpoint | | PSA-RE | |

## IDs not in PSA-RE

Our own observations (trip1 for driving, trip3 for a complete parked capture). All **hypotheses**, none confirmed.

| ID | Period, DLC | Behaviour | Best guess |
|----|-------------|-----------|------------|
| 302 | 20 ms, 7 | Constant while off. Driving: b1-b2 tracks `3AD` calibrated longitudinal acceleration (r = 0.93, ~16000 at rest); b5-b6 similar with less swing (0xEA60 = 60000 while off) | Acceleration / wheel torque request from the powertrain controller. Best lead |
| 40B | 50 ms, 4, only while ready | b0 = 127 most of the time, 20-80 at speed; b1-b2 jumps between a few discrete levels | Front camera: lane detection quality or a range, 127 = nothing detected |
| 42B / 44B | 50 ms, 8, only while ready | Identical layout. Parked: constant `7FFE 7FFE 7FE0 7F80` (looks invalid). Driving: signed 16-bit values near zero (b0-1, b2-3, ...), sometimes 0x8000 | Front camera lane lines, left (`42B`) / right (`44B`): offset, heading, curvature. Fits the lane keeping assist in `3F2` |
| 4A2 | 100 ms, 2 | `0040` while awake; `0080` from 0.2 s before the bus sleeps until it wakes (trip3: 23.8 s, sleep at 24 s) | Network management: awake / ready to sleep (b1 bits 6/7) |
| 352 | 100 ms, 2 | trip3: `FCF8` until 4.6 s after ready, then `1C6C` → `1E64` → `1A64`, kept after power-off | Unknown status word |
| 494 | ~1.5 s, 8 | Almost constant (`05D0760000040000`); only b3 bit 3 and b4 bits 6/4 ever change | Configuration or version broadcast |
| 66B | 5 frames, trip1 only | Around 24 s after the trip1 start; two alternating payloads with a page number in b0 high nibble | One-off message from an ECU at wake-up |

To test: `302` with a few hard accelerations and brakes at noted times. `40B`/`42B`/`44B` on a road with
clear lane lines, then without, plus a deliberate drift to one side to find the lateral offset.

### Network management, seen in trip3

- `782` (PSA-RE: SCDT_SUPERVISION) is each ECU's ~1 s network supervision frame. When one ECU wakes while
  the rest sleep, nobody ACKs it and it retransmits back to back (trip3: ~23,000 copies in 6.3 s). Treat
  such a burst as one frame in statistics.
- The bus can stay half-awake for a while after use (trip3: ~820 frames/s for 24 s after plugging in) and
  wakes fully on the brake pedal (trip2).

## Not found yet

- **A/C on/off button**: in trip3 the A/C came on by itself at ready (`3CE`: blower and evaporator setpoint at
  127-128 s). The user then switched it off and the A/C LED went out, but the evaporator kept cooling
  (14.4 → 0.5 °C) until power-off, and no bit on this bus changed cleanly at the right time. The button
  state is probably on the comfort bus. Why the evaporator kept cooling is open (battery cooling through
  the same refrigerant loop?).
- **Key-fob lights "off" press**: in trip3 the user pressed the lights button twice between locking and
  unlocking, and the lights went off. On this bus the lights stay on for exactly 30.0 s (267.2-297.2 s, past
  the unlock at 271.7) and the second press leaves no trace. So `40E`/`612` may be the light request
  rather than the lamp state, or the press didn't reach this bus.

- **Range (186 → 172 km)**: not in any field we searched, and PSA-RE has no range on HS1. Probably
  computed by the instrument cluster, or on another bus.
- **12 V battery SoC** (user: 92% before, 84% after): not found. The 12 V sensor (IBS) talks LIN to the
  body computer (`LIN` in PSA-RE). Voltage is available (`50D`).

- **Traffic sign IDs** (`59E` b0, b4): PSA-RE has no value table (plain number 1-254). In trip1 only 32
  `59E` frames survived, too few to map numbers to signs. Needs a complete capture plus notes of which
  signs were passed. Meanwhile `5F2` b2 (speed limit read by the camera, km/h) is directly usable.
- **Turn indicators** (`612` b1 bits 7-6): decoding fits trip1 (left → left turn), but right-indicator
  episodes at roundabouts make it a weak check. Confirm with an action log. It is the stalk request, not
  the lamp state.

## Ground truth from the user (trip1)

- Odometer after the trip: ~25,673 km (confirmed).
- Displayed SoC: 62% before, 53% after.
- No cruise control; **speed limiter** at 50 and 80 km/h (the user's usual settings are 30 / 50 / 80 / 100).
- Outside temperature 14-15 °C.
- Range on the dashboard: 186 km before, 172 km after.
- 12 V (service) battery: 92% before, 84% after.

## Ground truth from the user (trip2)

Times in seconds since logger boot, taken from the capture. The user confirmed the actions, not the times.

- 23.0: brake pedal pressed, car wakes; 25.2: ready.
- 44.8: P → D. 51.2: drive mode button (Normal → Eco).
- 60.2: accelerator touched in D, parking brake releases automatically; 64.9: parking brake switch pulled.
- 69.8: D → P. 85.5: car switched off.
- 138.3 / 166.6: driver door opened / closed. 170.1: locked from inside (confirmed); 177.3: unlocked (seen in the capture only).
- Odometer 25,707 km; the car did not move.

## Ground truth from the user (trip3)

Times in seconds since logger boot, from the capture. The user listed the actions and their order.

- 0–24: bus already awake (~820 frames/s) from earlier; asleep at 24 (LED blue).
- 34.9–41.3: one ECU woke alone and resent the same `782#5000000000000000` frame ~23,000 times (every 276 µs).
  No other node was awake to acknowledge it, and our node can't (listen-only). The 16 bus errors logged
  then (8 form + 8 bit) are its error frames until its TEC reached 128 (16 × 8) and it went error-passive.
  This is car behaviour, but tools should treat those frames as one.
- 123.0: bus wakes; 125.1 ready.
- 144.9: P → R; 150.2: R → D; 154.5 drive mode Normal → Eco (separate switch, pressed by the user).
- 157.0–158.2: parking brake auto-release; moving 159.3–175.5, max 5.9 km/h, **11.7 m** by wheel speeds
  (user: a very short drive, 10-15 m is plausible).
- 200.7: D → P, parking brake applied; 200.9: off.
- Doors open/closed: front left 205.4–220.0, rear left 222.1–224.0, rear right 228.9–230.3,
  front right 234.6–235.6, boot 245.9–249.9.
- 257.4: locked with key fob. 267.2: lights on from the fob, then a second press turned them off (user);
  the bus shows them off only at 297.2, a 30 s timer (see "Not found yet").
  271.7: unlocked with fob. 280.0–281.3: driver door. 283.1: locked (keyless); 290.5: unlocked (keyless).
  295.6: driver door opened; log ends at 306.3.
- A/C: came on by itself at ready, switched off by the user (LED out); the switch-off is not visible on
  this bus (see "Not found yet").
