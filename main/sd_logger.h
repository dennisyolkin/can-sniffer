#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint64_t timestamp_us;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    bool extended;
    bool remote;
} can_log_frame_t;

esp_err_t sd_logger_init(void);

// Appends one frame to can.log in candump format: "(sec.usec) can0 123#DEADBEEF".
esp_err_t sd_logger_log_can(const can_log_frame_t *frame);

// Appends one line to events.csv: logger/bus status, not CAN traffic.
esp_err_t sd_logger_log_event(uint64_t timestamp_us, const char *event, const char *value);

// Writes buffered data to the card. Call periodically; data not yet flushed is lost on power loss.
esp_err_t sd_logger_flush(void);

esp_err_t sd_logger_stop(void);
