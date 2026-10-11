# Decoded signals — Citroën ë-C4 (2022), OBD2 pins 6/14, 500 kbit/s

Working notes from reverse engineering. Byte numbering in this file is **b0..b7** as in candump (0-based);
PSA-RE uses **1-based** bytes (`3.7-4.0` = our b2..b3). Multi-byte fields are big-endian unless noted.

Status: **confirmed** = matches a dashboard reading; **verified** = our own analysis of the capture and
PSA-RE agree; **PSA-RE** = from PSA-RE and plausible on our data, not checked independently;
**ours** = our own finding, not in PSA-RE; **doubtful** = PSA-RE definition gives odd values on this car.

Sources:
- **trip1** (2026-10-11): ~32 min drive, 23 km, from cold start to park. Only 16.8% of frames were logged
  (logger stalls), so rare frames and exact timing are unreliable in it.
- **PSA-RE** ([github.com/prototux/PSA-RE](https://github.com/prototux/PSA-RE)): community database of PSA
  frames in YAML. The ë-C4 is car code `C41`, architecture `AEE2010.electric` (child of `.ev`, `.full`).
  Decode a capture with `tools/psare.py <session> <PSA-RE checkout>`; it resolves the variant chain.

## Which bus this is

The OBD pins 6/14 carry PSA's **HS1 (powertrain) bus**: 79 of our 87 IDs are defined in PSA-RE's
`AEE2010.{electric,ev,full}/HS.HS1`, and the decoded values make sense. Not in PSA-RE:
`302 352 40B 42B 44B 494 4A2 66B`.

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
| 349 b3, 489 b0, 4FE b1, 563 b3 | | Gear lever P / R / D | enum | PSA-RE | D 91 %, P 7-9 %, R 1 % |
| 3AD | b0, b3 | Electric parking brake state, force | enum | PSA-RE | Applied 6 % |
| 512 / 5E2 | | Drive mode (Eco 93-95 %, Normal), energy mode ZEV | enum | PSA-RE | |
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
| 572 | b0 | Seat belts (driver / passenger), reverse engaged | enum | PSA-RE | |
| 412 | b6 | Doors / boot status | | PSA-RE | All closed |
| 512 | b1 | Locked from inside / outside | enum | PSA-RE | "Super-locked" 92 % while driving? |
| 3CE | b1, b4-5 | Evaporator air / probe temperature, heater water | °C | PSA-RE | 12-14 °C |
| 56B / 5F2 / 59E | | Wipers, camera speed-sign reading (30/50/100), traffic-sign IDs | | PSA-RE | |
| 3F2 | b4-b7 | Lane keeping assist state, requested yaw rate, column setpoint | | PSA-RE | |

## Not found yet

- **Range (186 → 172 km)**: not in any field we searched, and PSA-RE has no range on HS1. Probably
  computed by the instrument cluster, or on another bus.
- **12 V battery SoC** (user: 92% before, 84% after): not found. The 12 V sensor (IBS) talks LIN to the
  body computer (`LIN` in PSA-RE). Voltage is available (`50D`).

## Ground truth from the user (trip1)

- Odometer after the trip: ~25,673 km (confirmed).
- Displayed SoC: 62% before, 53% after.
- No cruise control; **speed limiter** at 50 and 80 km/h (the user's usual settings are 30 / 50 / 80 / 100).
- Outside temperature 14-15 °C.
- Range on the dashboard: 186 km before, 172 km after.
- 12 V (service) battery: 92% before, 84% after.
