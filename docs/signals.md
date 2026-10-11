# Decoded signals — Citroën ë-C4 (2022), OBD2 pins 6/14, 500 kbit/s

Working notes from reverse engineering. Byte numbering is b0..b7 as in candump; multi-byte fields are
big-endian unless noted. Confidence: **high** = matches physics or a known reading; **med** = fits the
data well but is not checked against the car; **low** = guess.

Sources:
- **trip1** (2026-10-11): ~32 min drive, 23 km, from cold start to park. Only 16.8% of frames were logged
  (logger stalls, see the pending fix), so rare frames and exact timing are unreliable in it.

## Bus overview (trip1)

87 IDs, all 11-bit, ~1,550 frames/s, no bus errors. Periods: 10 ms (`208 262 2F5 305 349 3CD`),
20 ms (`2EB 302 30D 334 348 34D 3AD 40D`), 40 ms, 50 ms, 100 ms, plus slow IDs (~1 s) in the 5xx/7xx range.
Many IDs have a rolling counter in a nibble (`CNT` in `tools/bytestats.py` output).

## Signals

| ID | Bits | Decode | Meaning | Conf. | Evidence |
|----|------|--------|---------|-------|----------|
| 30D | b0-1, b2-3, b4-5, b6-7 | ×0.01 km/h | Four wheel speeds | high | 0 at stops; integrates to 23.4 km = odometer delta. Speed limiter at 50 / 80 (dash) gives 48.0 / 77.5 km/h here: the dash reads 3-4% high |
| 44D | b0-1, b2-3, b4-5 | ×0.01 km/h | Wheel speeds again (other ECU?) | high | Same as 30D |
| 44D | b6-7 | ≈1.95 × speed | Unknown, proportional to speed | med | r = 1.000 with 30D |
| 38D | b0-1 | ×0.01 km/h | Vehicle speed | high | Same as 30D |
| 305 | b0-1 signed | ×0.1 ° | Steering wheel angle | high | Smooth, saturates at ±~500° during parking |
| 2F5 | b3-4 signed | ×0.1 ° | Steering wheel angle (copy) | high | Identical to 305 b0-1 |
| 2F5 | b5-6 signed | ? | Steering angle rate | med | Sign follows the angle's derivative |
| 305 | b2 (= b5) | ? | Steering rate, unsigned | med | Spikes on fast wheel turns |
| 2F5 | b1-2 signed | ? | Steering torque (driver / EPS)? | low | Noisy, follows turning effort |
| 3CD | b2-3 signed | ? | Yaw rate | med | Large when turning while moving, ~0 when turning at standstill |
| 3CD | b0-1 signed | ? | Lateral acceleration? | low | Same shape, smaller |
| 208 | b5 signed | ? | Motor torque | med | Negative when braking (regen), +16 at standstill in D (creep), 0 after parking |
| 208 | b3 | 0..116, scale unknown | Accelerator pedal position | med | 0 when stopped; during limiter plateaus it drifts slowly like a foot (e.g. 70-81) while torque (b5) stays small |
| 552 | b0-3 | ×0.1 s | Running clock (~24.6 h at trip start) | high | +19,205 over 1,924 s |
| 552 | b4-6 | km | **Odometer** | confirmed | 25,650 → 25,673 km; the user confirmed the dashboard value |
| 3A8 | b3 | % | **Displayed SoC** | confirmed | 62 → 53, same as the dashboard |
| 3A8 | b4 | ×0.5 % | Displayed SoC | confirmed | 124 → 106 = 62 → 53 % |
| 5C3 | b2[1:0] + b3 (10 bits) | ×0.1 % | Displayed SoC, fine | confirmed | 620 → 532 = 62.0 → 53.2 % |
| 543 | b0 (b0-1 /256?) | % | **Second SoC, 6 points higher** (BMS raw SoC?) | med | 68 → 59; b0-1 is smooth: 68.25 → 59.75 if /256 |
| 563 | b4[4:0] + b5 | ? | Decreasing with driving: energy? | low | 4264 → 3658, same shape as SoC. Not the range (dash showed 186 → 172) |
| 3F4 | b0[7:5] = page 0-7; b1..b7 | raw - 40 = °C? | **Battery temperatures, multiplexed**: 8 pages × 7 values, the last 2 of page 7 unused = 54 sensors | med | All 52-53 (12-13 °C) with 14-15 °C outside; the pack barely warms on a 23 km drive. Page selector steps 0..7 in b0's top 3 bits |
| 522 | b0 | raw - 40 = °C? | Temperature, rising | low | 56 → 73 |
| 5C1 | b5, b6 | raw - 40 = °C? | Temperatures, rising | low | 52 → 67, 52 → 72 |
| 3CE | b5 | °C? | Temperature, flat at 15, then rising from ~750 s | low | 15 → 33 |
| 583 | b0 | ? | Steps down 23 → 0 during the trip | low | |
| 583 | b5 (raw - 40) / b6 (×0.5 - 40) | °C? | Outside temperature? Both decode to 14 °C | low | Constant for the whole trip; outside was 14-15 °C |

## Ground truth from the user (trip1)

- Odometer after the trip: ~25,673 km (confirmed).
- Displayed SoC: 62% before, 53% after.
- No cruise control; **speed limiter** at 50 and 80 km/h (the user's usual settings are 30 / 50 / 80 / 100).
- Outside temperature 14-15 °C.
- Range on the dashboard: 186 km before, 172 km after.
- 12 V (service) battery: 92% before, 84% after.

## Not found yet

- **Range (186 → 172)**: no field of 8-16 bits, at any bit offset, scale 0.5/1/2/4/5/10, goes 186 → 172.
  The cluster may compute it itself, or it is on another bus.
- Outside temperature: only weak candidates (583). A trip with a different temperature would tell.
- **12 V battery SoC** (user: 92% before, 84% after): no field goes 92 → 84 during the trip. Odd for an
  EV, where the DC/DC converter charges the 12 V battery while driving. The 12 V sensor (IBS) usually talks
  LIN to the body computer, so it may never appear on this bus, or only while the car is asleep/waking.
  Weak lead: `0AE` b3 = 92 (0x5C), but `0AE` appeared only twice (identical) at 19.4 s, when the car
  became ready.
- **12 V voltage**: not identified. Easiest to find with a capture covering "unlocked, not ready"
  (~12.5 V) → ready (DC/DC on, ~14 V): look for a step at that moment.
