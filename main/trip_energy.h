#pragma once

#include <stdint.h>

#include "sd_logger.h"

// Energy drawn from and regenerated into the traction battery since the last reset, added up from the car's 3A8
// counters (docs/signals.md). Runs in the logger loop on every frame, so nothing is missed while no page is open.

typedef struct {
    int32_t drawn_wh;
    int32_t regen_wh;
    int32_t net_wh;      // Drawn − regenerated, from the car's own net counter.
    uint32_t cycles;     // Drive cycles the car started (its counters reset to 0 / 0 / 254) since the trip began.
    uint32_t gaps;       // 3A8 resumed after a pause with changed values: whole wraps (256 Wh) may be missing.
    int64_t started_us;  // Board time of the reset (esp_timer); 0 = since boot.
    int32_t km_start;    // Odometer (552) at the start of the trip, −1 until known.
    int32_t km_now;      // −1 until known.
} trip_energy_t;

// Logger loop side.
void trip_energy_update(const can_log_frame_t *frame);
void trip_energy_reset(int64_t now_us);
void trip_energy_get(trip_energy_t *out);
