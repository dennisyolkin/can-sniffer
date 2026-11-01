#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_twai.h"

#include "sd_logger.h"

// A fixed list of diagnostic requests (UDS 0x3E/0x22/0x19, 0x10 default/extended session, OBD-II mode 01), sent one
// at a time on request from the page. Nothing that writes, resets or unlocks the ECU. Only in the CONFIG_CAN_TX_MODE build. Requests are single frames; replies up to UDS_CLIENT_MSG_MAX bytes
// are reassembled with ISO-TP (first frame, our flow control, consecutive frames).

#define UDS_CLIENT_LOG_LEN 16
// ISO-TP allows 4095. The longest DID we read is 408 bytes (BMS D465); longer replies are not followed.
#define UDS_CLIENT_MSG_MAX 512

typedef enum {
    UDS_LOG_TX,         // Request sent (data = the frame).
    UDS_LOG_RX,         // Reply received (data = the frame).
    UDS_LOG_TX_FAILED,  // Every attempt failed (arbitration lost or bus error); len = attempts.
    UDS_LOG_DONE,       // Reply window closed; len = number of replies.
    UDS_LOG_REFUSED,    // Not sent: bus not error_active.
    UDS_LOG_MESSAGE,    // Multi-frame reply complete; len = its length (uds_client_get_message has the bytes).
    UDS_LOG_RX_ERROR,   // Multi-frame reply abandoned; len = uds_rx_error_t.
} uds_log_kind_t;

typedef enum {
    UDS_RX_ERROR_SEQUENCE,    // A consecutive frame was missing or out of order.
    UDS_RX_ERROR_TOO_LONG,    // Longer than UDS_CLIENT_MSG_MAX; no flow control sent.
    UDS_RX_ERROR_INCOMPLETE,  // The reply window closed before the last consecutive frame.
} uds_rx_error_t;

// The latest reassembled multi-frame reply.
typedef struct {
    uint32_t seq;  // 0 until the first one; changes with every new message.
    uint32_t t_ms;
    uint8_t request;
    uint16_t id;
    uint16_t len;
    uint8_t data[UDS_CLIENT_MSG_MAX];  // The UDS payload (service byte first), without ISO-TP headers.
} uds_message_t;

typedef struct {
    uint32_t t_ms;
    uint8_t kind;  // uds_log_kind_t
    uint8_t request;  // Index into the request table.
    uint16_t id;
    uint16_t len;
    uint8_t data[8];
} uds_log_entry_t;

#define UDS_CLIENT_PARAM_MAX 3

typedef struct {
    const char *name;   // Command name from the page: "uds <name>" or "uds <name> <hex param>".
    const char *label;  // Shown on the button.
    uint16_t tx_id;
    uint16_t rx_id_min;
    uint16_t rx_id_max;
    uint8_t data[8];    // ISO-TP single frame: length byte, service, parameters, padding.
    uint8_t param_offset;  // Where the page's parameter goes in data (a DTC for 0x19 04/06)...
    uint8_t param_len;     // ...and how many bytes it must have; 0 = no parameter.
    bool danger;           // Changes the ECU's state (0x10); the page shows it in red.
} uds_request_t;

esp_err_t uds_client_init(twai_node_handle_t node);

// Logger loop side.
void uds_client_on_frame(const can_log_frame_t *frame);
// Sends a pending request, retries a failed one and closes the reply window. bus_ok: node is error_active.
void uds_client_poll(int64_t now_us, bool bus_ok);

// TWAI ISR side (on_tx_done).
void uds_client_on_tx_done_from_isr(bool success);

// Web server side.
// param: param_len bytes for a request that takes one, else NULL/0. ESP_ERR_NOT_FOUND for an unknown name,
// ESP_ERR_INVALID_ARG for a parameter of the wrong length, ESP_ERR_INVALID_STATE while a request is in flight.
esp_err_t uds_client_request(const char *name, const uint8_t *param, size_t param_len);
size_t uds_client_request_count(void);
const uds_request_t *uds_client_get_request(size_t index);
// Copies the log oldest first; returns the entry count. seq changes whenever an entry is added.
size_t uds_client_get_log(uds_log_entry_t *out, size_t max, uint32_t *seq);
void uds_client_get_message(uds_message_t *out);
