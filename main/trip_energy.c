#include "trip_energy.h"

#include <stdbool.h>

#define ENERGY_ID 0x3A8
#define ODOMETER_ID 0x552
#define ODOMETER_INVALID 0xFFFFFF
// 3A8 is sent every 0.5 s while the car is awake. After a longer pause (the car slept, or frames were lost) the
// counters normally resume where they stopped; if they moved, any whole wrap in between is lost.
#define GAP_US (2 * 1000 * 1000)

// What the car sets b0 / b1 / b2 to when a drive cycle starts (trip-20261104: at ready, and at a wake-up).
static const uint8_t reset_values[3] = {0, 0, 254};

// Logger loop only.
static trip_energy_t totals = {.km_start = -1, .km_now = -1};
static bool have_last;
static uint8_t last[3];
static int64_t last_us;

// Each counter is the low byte of a larger total, so a step is taken modulo 256 (−128..127).
static int wrap_delta(uint8_t now, uint8_t before) {
    return (int) ((now - before + 384) % 256) - 128;
}

static void on_energy(const can_log_frame_t *frame) {
    const uint8_t *b = frame->data;
    if (have_last) {
        bool changed = b[0] != last[0] || b[1] != last[1] || b[2] != last[2];
        bool reset = changed && b[0] == reset_values[0] && b[1] == reset_values[1] && b[2] == reset_values[2];
        if (reset) {
            totals.cycles++;
        } else {
            if (changed && frame->timestamp_us - last_us > GAP_US) {
                totals.gaps++;
            }
            int drawn = wrap_delta(b[1], last[1]);
            int regen = -wrap_delta(b[2], last[2]);  // The regenerated counter counts down.
            totals.net_wh += wrap_delta(b[0], last[0]);
            totals.drawn_wh += drawn > 0 ? drawn : 0;
            totals.regen_wh += regen > 0 ? regen : 0;
        }
    }
    last[0] = b[0];
    last[1] = b[1];
    last[2] = b[2];
    last_us = frame->timestamp_us;
    have_last = true;
}

static void on_odometer(const can_log_frame_t *frame) {
    int32_t km = (frame->data[4] << 16) | (frame->data[5] << 8) | frame->data[6];
    if (km == ODOMETER_INVALID) {
        return;
    }
    totals.km_now = km;
    if (totals.km_start < 0) {
        totals.km_start = km;
    }
}

void trip_energy_update(const can_log_frame_t *frame) {
    if (frame->extended || frame->remote) {
        return;
    }
    if (frame->id == ENERGY_ID && frame->dlc >= 3) {
        on_energy(frame);
    } else if (frame->id == ODOMETER_ID && frame->dlc >= 7) {
        on_odometer(frame);
    }
}

// The 3A8 baseline stays: the next frame's change counts towards the new trip.
void trip_energy_reset(int64_t now_us) {
    totals = (trip_energy_t) {
        .started_us = now_us,
        .km_start = totals.km_now,
        .km_now = totals.km_now,
    };
}

void trip_energy_get(trip_energy_t *out) {
    *out = totals;
}
