#pragma once

#include <stdint.h>

#include "sd_logger.h"

// Energy since the last reset, two ways (docs/signals.md), in the logger loop on every frame so nothing is missed while
// no page is open:
// - the car's 3A8 counters: traction and the 12 V side, not the climate system (20261122);
// - the battery itself: 563 remaining Ah × 5C3 pack voltage, everything including heating.

typedef struct {
    int32_t drawn_wh;
    int32_t regen_wh;
    int32_t net_wh;      // Drawn − regenerated, from the car's own net counter.
    uint32_t cycles;     // Drive cycles the car started (its counters reset to 0 / 0 / 254) since the trip began.
    uint32_t gaps;       // 3A8 resumed after a pause with changed values: whole wraps (256 Wh) may be missing.
    int32_t battery_out_wh;  // 563 × 5C3: taken out of the battery (remaining Ah falling)...
    int32_t battery_in_wh;   // ...and put back (rising: regen, charging).
    uint32_t battery_skips;  // 563 steps over 1 Ah left out (single-frame glitches, or a jump after a long pause).
    int64_t started_us;  // Board time of the reset (esp_timer); 0 = since boot.
    int32_t km_start;    // Odometer (552) at the start of the trip, −1 until known.
    int32_t km_now;      // −1 until known.
} trip_energy_t;

// Logger loop side.
void trip_energy_update(const can_log_frame_t *frame);
void trip_energy_reset(int64_t now_us);
void trip_energy_get(trip_energy_t *out);
