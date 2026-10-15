#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "sd_logger.h"

#define LIVE_DATA_MARK_LABEL_MAX 32

// Latest frame of one ID, or of one page of a muxed ID. Sent to the page as is (see docs/live-view.md), so the
// layout is part of the WebSocket protocol.
typedef struct {
    uint16_t id;        // Bit 15: remote frame.
    uint8_t page;       // LIVE_DATA_NO_PAGE unless the ID is muxed.
    uint8_t dlc;
    uint32_t count;     // Frames received with this key since boot.
    uint32_t last_ms;   // Receive time of the latest one, ms since boot.
    uint8_t data[8];
} live_frame_t;

_Static_assert(sizeof(live_frame_t) == 20, "live_frame_t is a wire format");

#define LIVE_DATA_NO_PAGE 0xFF
#define LIVE_DATA_REMOTE_FLAG 0x8000

typedef enum {
    LIVE_RECORDER_NO_CARD,
    LIVE_RECORDER_OFF,
    LIVE_RECORDER_ON,
    LIVE_RECORDER_FAILED,
} live_recorder_state_t;

// Published by the logger loop once per second.
typedef struct {
    uint32_t frames_per_s;
    uint32_t frames_total;
    uint32_t rx_dropped_total;
    uint32_t bus_errors_total;
    uint32_t bus_errors_per_s;
    live_recorder_state_t recorder;
    char session[SD_LOGGER_SESSION_NAME_MAX];
    uint32_t sd_dropped;
    uint32_t buffers_min_free;
    uint32_t buffer_count;
    uint32_t marks;  // Marks written in this session.
} live_status_t;

typedef struct {
    int64_t timestamp_us;
    char label[LIVE_DATA_MARK_LABEL_MAX];
} live_mark_t;

esp_err_t live_data_init(void);

// Logger loop side. live_data_update runs for every frame: a table lookup and a short copy under a spinlock.
void live_data_update(const can_log_frame_t *frame);
void live_data_set_status(const live_status_t *status);
// Return true when the web page asked for something since the last call.
bool live_data_take_recording_request(bool *on);
bool live_data_take_mark(live_mark_t *mark);

// Web server side.
// Copies up to `max` entries; returns how many there are.
size_t live_data_snapshot(live_frame_t *out, size_t max);
void live_data_get_status(live_status_t *status);
void live_data_request_recording(bool on);
// ESP_ERR_NO_MEM when marks arrive faster than the logger loop takes them (it does once per second).
esp_err_t live_data_add_mark(const char *label);
