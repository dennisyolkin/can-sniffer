#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_twai.h"

#include "sd_logger.h"

// A fixed list of read-only diagnostic requests (UDS 0x3E/0x22, OBD-II mode 01), sent one at a time on request from
// the page. Only in the CONFIG_CAN_TX_MODE build. Single-frame requests only; replies longer than one frame are
// logged but not followed (no ISO-TP flow control).

#define UDS_CLIENT_LOG_LEN 16

typedef enum {
    UDS_LOG_TX,         // Request sent (data = the frame).
    UDS_LOG_RX,         // Reply received (data = the frame).
    UDS_LOG_TX_FAILED,  // Every attempt failed (arbitration lost or bus error); len = attempts.
    UDS_LOG_DONE,       // Reply window closed; len = number of replies.
    UDS_LOG_REFUSED,    // Not sent: bus not error_active.
} uds_log_kind_t;

typedef struct {
    uint32_t t_ms;
    uint8_t kind;  // uds_log_kind_t
    uint8_t request;  // Index into the request table.
    uint16_t id;
    uint8_t len;
    uint8_t data[8];
} uds_log_entry_t;

typedef struct {
    const char *name;   // Command name from the page: "uds <name>".
    const char *label;  // Shown on the button.
    uint16_t tx_id;
    uint16_t rx_id_min;
    uint16_t rx_id_max;
    uint8_t data[8];    // ISO-TP single frame: length byte, service, parameters, padding.
} uds_request_t;

esp_err_t uds_client_init(twai_node_handle_t node);

// Logger loop side.
void uds_client_on_frame(const can_log_frame_t *frame);
// Sends a pending request, retries a failed one and closes the reply window. bus_ok: node is error_active.
void uds_client_poll(int64_t now_us, bool bus_ok);

// TWAI ISR side (on_tx_done).
void uds_client_on_tx_done_from_isr(bool success);

// Web server side.
// ESP_ERR_NOT_FOUND for an unknown name, ESP_ERR_INVALID_STATE while a request is in flight.
esp_err_t uds_client_request(const char *name);
size_t uds_client_request_count(void);
const uds_request_t *uds_client_get_request(size_t index);
// Copies the log oldest first; returns the entry count. seq changes whenever an entry is added.
size_t uds_client_get_log(uds_log_entry_t *out, size_t max, uint32_t *seq);
