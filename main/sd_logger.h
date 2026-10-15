#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// "<boot id>-<sequence>", e.g. "1a2b3c4d-01", plus terminator.
#define SD_LOGGER_SESSION_NAME_MAX 20

typedef struct {
    uint64_t timestamp_us;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    bool extended;
    bool remote;
} can_log_frame_t;

typedef struct {
    uint32_t dropped_frames;    // Frames lost in this session because every write buffer was waiting for the card.
    uint32_t write_max_us;      // Slowest single write since the previous call.
    uint32_t sync_max_us;       // Slowest fsync since the previous call.
    uint32_t buffers_min_free;  // Fewest idle write buffers since the previous call.
    uint32_t buffer_count;
} sd_logger_stats_t;

// Mounts the card, allocates the write buffers and starts the writer task. No session is open afterwards.
esp_err_t sd_logger_init(void);

// Session control and the log functions below are called from one task only (the logger loop).

// Opens a new session directory, /sdcard/can-sniff/<name>/ with can.log and events.csv. The card work happens in
// the writer task, so a failure to create the files shows up later through sd_logger_failed().
// `name` receives the session name.
esp_err_t sd_logger_start(char name[SD_LOGGER_SESSION_NAME_MAX]);

// Hands everything buffered to the writer, which syncs and closes the files.
esp_err_t sd_logger_stop(void);

bool sd_logger_recording(void);

// True when writing the current session failed (card full or removed). Nothing more is written until a new session.
bool sd_logger_failed(void);

// The log functions only format into RAM and never block; a background task writes to the card.
// ESP_ERR_INVALID_STATE: no session. ESP_ERR_NO_MEM: no free buffer, the line was dropped.
// ESP_FAIL: writing this session failed earlier, see sd_logger_failed().

// Appends one frame to can.log in candump format: "(sec.usec) can0 123#DEADBEEF".
esp_err_t sd_logger_log_can(const can_log_frame_t *frame);

// Appends one line to events.csv: logger/bus status, not CAN traffic.
esp_err_t sd_logger_log_event(uint64_t timestamp_us, const char *event, const char *value);

// Hands partly filled buffers to the writer. Call periodically; the writer fsyncs about once per second,
// so data still in RAM or not yet synced is lost on power loss.
esp_err_t sd_logger_flush(void);

// Reads the counters; the *_max and min fields restart with each call.
esp_err_t sd_logger_get_stats(sd_logger_stats_t *stats);
