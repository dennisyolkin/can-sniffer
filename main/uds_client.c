#include "uds_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_attr.h"
#include "esp_log.h"

// Long enough for an ECU to answer (UDS P2 is 50 ms, plus "response pending") and for every ECU to answer a
// functional OBD-II request.
#define REPLY_WINDOW_US (1000 * 1000)
// The ESP32 controller only has single-shot or endless retransmission (twai_hal_v1.c), so a request that loses
// arbitration is retried here, a bounded number of times. Car ECUs send in bursts on their 10 ms ticks, and our
// diagnostic IDs lose to all of them: back-to-back retries (tx-logs-2) all landed in the same burst.
#define MAX_ATTEMPTS 5
#define RETRY_DELAY_US (5 * 1000)

#define UDS_NRC 0x7F
#define UDS_NRC_RESPONSE_PENDING 0x78

// ISO-TP (ISO 15765-2) frame types: high nibble of the first byte.
#define ISOTP_SINGLE 0x0
#define ISOTP_FIRST 0x1
#define ISOTP_CONSECUTIVE 0x2
// Our flow control after a first frame: "continue to send", block size 0 (all consecutive frames without another
// flow control), and at least 1 ms between them so a long reply doesn't arrive as one burst.
#define ISOTP_FC_CONTINUE 0x30
#define ISOTP_FC_BLOCK_SIZE 0x00
#define ISOTP_FC_ST_MIN_MS 0x01

static const char *TAG = "uds_client";

// IDs: PSA-RE diag/AEE2010.ev. They never appear in the car's own traffic (checked against trip3 and the test
// captures), so a request cannot collide with an ECU's frame. Padding is 0x00.
// Only ECUs that answered from the OBD port (tx-logs-2) are listed; the BSI, the cluster and OBD-II never did
// (docs/live-view.md, "Transmitting").
// BMS reads: DIDs from PSA-RE TBMU/BMU_CTE1. Values up to 4 bytes fit in a single-frame reply; longer ones come back
// as ISO-TP multi-frame replies. The page decodes them; the label's "ECU:" prefix groups the buttons.
#define BMS_READ(name, label, did) \
    {name, "BMS: " label, 0x6B4, 0x694, 0x694, {0x03, 0x22, (uint8_t) ((did) >> 8), (uint8_t) ((did) & 0xFF)}}

static const uds_request_t requests[] = {
    {"vcu_tp", "VCU: TesterPresent", 0x6A2, 0x682, 0x682, {0x02, 0x3E, 0x00}},
    {"abs_tp", "ABS: TesterPresent", 0x6AD, 0x68D, 0x68D, {0x02, 0x3E, 0x00}},
    {"bms_tp", "BMS: TesterPresent", 0x6B4, 0x694, 0x694, {0x02, 0x3E, 0x00}},
    BMS_READ("bms_d810", "SoC", 0xD810),
    BMS_READ("bms_d860", "SoH", 0xD860),
    BMS_READ("bms_d413", "Min cell capacity", 0xD413),
    BMS_READ("bms_d865", "Energy", 0xD865),
    BMS_READ("bms_d815", "Pack voltage", 0xD815),
    BMS_READ("bms_d816", "Current", 0xD816),
    BMS_READ("bms_d43d", "Avg cell V", 0xD43D),
    BMS_READ("bms_d86f", "Lowest cell V", 0xD86F),
    BMS_READ("bms_d870", "Highest cell V", 0xD870),
    BMS_READ("bms_d43c", "Lowest cell no.", 0xD43C),
    BMS_READ("bms_d43b", "Highest cell no.", 0xD43B),
    BMS_READ("bms_d877", "Avg temp", 0xD877),
    BMS_READ("bms_d87d", "Min temp", 0xD87D),
    BMS_READ("bms_d817", "Max temp", 0xD817),
    BMS_READ("bms_d878", "Temp spread", 0xD878),
    BMS_READ("bms_d871", "Charge limit 10 s", 0xD871),
    BMS_READ("bms_d873", "Discharge limit 10 s", 0xD873),
    BMS_READ("bms_d811", "Battery status", 0xD811),
    BMS_READ("bms_d86d", "Balancing", 0xD86D),
    BMS_READ("bms_d47a", "Insulation", 0xD47A),
    BMS_READ("bms_d822", "12 V battery", 0xD822),
    BMS_READ("bms_f18b", "Manufacturing date", 0xF18B),
    // Multi-frame replies.
    BMS_READ("bms_d4b5", "Cell SoH (216 B)", 0xD4B5),
    BMS_READ("bms_f080", "ECU identification", 0xF080),
    BMS_READ("bms_f0fe", "Software", 0xF0FE),
    BMS_READ("bms_d901", "Serial number", 0xD901),
    BMS_READ("bms_d465", "D465 (408 B)", 0xD465),
};
#define REQUEST_COUNT (sizeof(requests) / sizeof(requests[0]))

typedef enum {
    STATE_IDLE,
    STATE_SENDING,  // Waiting for on_tx_done.
    STATE_BACKOFF,  // An attempt failed; waiting before the next one.
    STATE_WAITING,  // Sent; collecting replies until the window closes.
} state_t;

// What STATE_SENDING / STATE_BACKOFF are sending.
typedef enum {
    TX_KIND_REQUEST,
    TX_KIND_FLOW_CONTROL,
} tx_kind_t;

typedef enum {
    TX_RESULT_NONE,
    TX_RESULT_OK,
    TX_RESULT_FAILED,
} tx_result_t;

static twai_node_handle_t node;

// Web server -> logger loop.
static atomic_int pending = -1;
static atomic_bool busy;
// ISR -> logger loop.
static atomic_int tx_result;

// Logger loop only.
static state_t state;
static const uds_request_t *current;
static unsigned attempts;
static unsigned replies;
static bool final_reply;
static int64_t window_end_us;
static int64_t retry_at_us;
static tx_kind_t tx_kind;
static uint16_t tx_id;
// The driver keeps a pointer to the frame until it is sent.
static twai_frame_t tx_frame;
static uint8_t tx_data[8];

// Multi-frame reply being reassembled (logger loop only).
static bool isotp_active;
static bool flow_control_due;
static uint16_t isotp_len;
static uint16_t isotp_received;
static uint8_t isotp_next_sn;
static uint16_t isotp_id;
static uint8_t isotp_data[UDS_CLIENT_MSG_MAX];
// "<name> <hex>" for events.csv; static, it is too big for the logger loop's stack.
static char message_text[32 + 2 * UDS_CLIENT_MSG_MAX];

// Shared with the web server.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uds_log_entry_t log_entries[UDS_CLIENT_LOG_LEN];
static unsigned log_head;
static unsigned log_count;
static uint32_t log_seq;
static uds_message_t message;

// Read-only services only, single frames only. Checked in C so a change to the table cannot slip past it.
static bool allowed(const uds_request_t *r) {
    uint8_t len = r->data[0];
    if (len < 2 || len > 7) {
        return false;
    }
    switch (r->data[1]) {
    case 0x3E:  // TesterPresent, with or without "suppress positive response".
        return len == 2 && (r->data[2] == 0x00 || r->data[2] == 0x80);
    case 0x22:  // ReadDataByIdentifier: one or more 2-byte DIDs.
        return len >= 3 && (len - 1) % 2 == 0;
    case 0x01:  // OBD-II current data, functional or physical request.
        return len >= 2 && (r->tx_id == 0x7DF || (r->tx_id >= 0x7E0 && r->tx_id <= 0x7E7));
    default:
        return false;
    }
}

static void add_log(uds_log_kind_t kind, uint32_t t_ms, uint16_t id, uint16_t len, const uint8_t *data) {
    uds_log_entry_t entry = {
        .t_ms = t_ms,
        .kind = kind,
        .request = (uint8_t) (current - requests),
        .id = id,
        .len = len,
    };
    if (data != NULL) {
        memcpy(entry.data, data, len < sizeof(entry.data) ? len : sizeof(entry.data));
    }
    taskENTER_CRITICAL(&lock);
    log_entries[(log_head + log_count) % UDS_CLIENT_LOG_LEN] = entry;
    if (log_count < UDS_CLIENT_LOG_LEN) {
        log_count++;
    } else {
        log_head = (log_head + 1) % UDS_CLIENT_LOG_LEN;
    }
    log_seq++;
    taskEXIT_CRITICAL(&lock);
}

// events.csv only has room for text; the frame goes in candump notation. Only `len` bytes: the receive buffer holds
// stale bytes from earlier frames past the DLC, and ECUs often send short, unpadded replies.
static void log_frame_event(uint64_t timestamp_us, const char *event, uint16_t id, uint8_t len, const uint8_t *data) {
    char text[24];
    int pos = snprintf(text, sizeof(text), "%03X#", id);
    for (uint8_t i = 0; i < len && i < 8; i++) {
        pos += snprintf(text + pos, sizeof(text) - pos, "%02X", data[i]);
    }
    // Fails when not recording; the page log still has it.
    (void) sd_logger_log_event(timestamp_us, event, text);
}

static void log_text_event(uint64_t timestamp_us, const char *event, const char *what, unsigned n) {
    char text[48];
    snprintf(text, sizeof(text), "%s %u", what, n);
    (void) sd_logger_log_event(timestamp_us, event, text);
}

static void finish(void) {
    state = STATE_IDLE;
    atomic_store(&busy, false);
}

static void fail(int64_t now_us) {
    add_log(UDS_LOG_TX_FAILED, now_us / 1000, current->tx_id, attempts, NULL);
    log_text_event(now_us, "uds_tx_failed", current->name, attempts);
    finish();
}

// Sends tx_data (already filled) on tx_id; retries go through STATE_BACKOFF.
static void transmit(int64_t now_us) {
    attempts++;
    atomic_store(&tx_result, TX_RESULT_NONE);
    tx_frame = (twai_frame_t) {
        .header.id = tx_id,
        .buffer = tx_data,
        .buffer_len = sizeof(tx_data),
    };
    window_end_us = now_us + REPLY_WINDOW_US;
    if (twai_node_transmit(node, &tx_frame, 0) != ESP_OK) {
        atomic_store(&tx_result, TX_RESULT_FAILED);
    }
    state = STATE_SENDING;
    add_log(UDS_LOG_TX, now_us / 1000, tx_id, sizeof(tx_data), tx_data);
    log_frame_event(now_us, tx_kind == TX_KIND_REQUEST ? "uds_tx" : "uds_fc", tx_id, sizeof(tx_data), tx_data);
}

static void start_request(int64_t now_us) {
    tx_kind = TX_KIND_REQUEST;
    tx_id = current->tx_id;
    memcpy(tx_data, current->data, sizeof(tx_data));
    attempts = 0;
    transmit(now_us);
}

// Only ever in answer to a first frame from the ECU we just asked, on that request's ID.
static void start_flow_control(int64_t now_us) {
    tx_kind = TX_KIND_FLOW_CONTROL;
    tx_id = current->tx_id;
    memset(tx_data, 0, sizeof(tx_data));
    tx_data[0] = ISOTP_FC_CONTINUE;
    tx_data[1] = ISOTP_FC_BLOCK_SIZE;
    tx_data[2] = ISOTP_FC_ST_MIN_MS;
    attempts = 0;
    transmit(now_us);
}

static void rx_error(uint64_t timestamp_us, uds_rx_error_t error) {
    static const char *const names[] = {
        [UDS_RX_ERROR_SEQUENCE] = "sequence",
        [UDS_RX_ERROR_TOO_LONG] = "too_long",
        [UDS_RX_ERROR_INCOMPLETE] = "incomplete",
    };
    isotp_active = false;
    flow_control_due = false;
    add_log(UDS_LOG_RX_ERROR, timestamp_us / 1000, isotp_id, error, NULL);
    char text[48];
    snprintf(text, sizeof(text), "%s %s %u/%u", current->name, names[error], isotp_received, isotp_len);
    (void) sd_logger_log_event(timestamp_us, "uds_rx_error", text);
}

static void message_complete(uint64_t timestamp_us) {
    isotp_active = false;
    taskENTER_CRITICAL(&lock);
    message.seq++;
    message.t_ms = timestamp_us / 1000;
    message.request = (uint8_t) (current - requests);
    message.id = isotp_id;
    message.len = isotp_len;
    memcpy(message.data, isotp_data, isotp_len);
    taskEXIT_CRITICAL(&lock);
    add_log(UDS_LOG_MESSAGE, timestamp_us / 1000, isotp_id, isotp_len, NULL);

    int pos = snprintf(message_text, sizeof(message_text), "%s ", current->name);
    for (uint16_t i = 0; i < isotp_len; i++) {
        pos += snprintf(message_text + pos, sizeof(message_text) - pos, "%02X", isotp_data[i]);
    }
    (void) sd_logger_log_event(timestamp_us, "uds_msg", message_text);
}

// First frame: 1L LL + 6 payload bytes, where LLL is the total payload length.
static void on_first_frame(const can_log_frame_t *frame) {
    isotp_id = frame->id;
    isotp_len = ((frame->data[0] & 0x0F) << 8) | frame->data[1];
    isotp_received = 0;
    if (frame->dlc < 8 || isotp_len <= 7) {
        return;  // Malformed; a reply this short would have been a single frame.
    }
    if (isotp_len > UDS_CLIENT_MSG_MAX) {
        // Without flow control the ECU gives up by itself (N_Bs timeout).
        rx_error(frame->timestamp_us, UDS_RX_ERROR_TOO_LONG);
        final_reply = true;
        return;
    }
    memcpy(isotp_data, &frame->data[2], 6);
    isotp_received = 6;
    isotp_next_sn = 1;
    isotp_active = true;
    flow_control_due = true;
}

// Consecutive frame: 2N + up to 7 payload bytes, N counting 1..15, 0, 1...
static void on_consecutive_frame(const can_log_frame_t *frame) {
    if (!isotp_active || frame->id != isotp_id) {
        return;
    }
    if ((frame->data[0] & 0x0F) != isotp_next_sn) {
        rx_error(frame->timestamp_us, UDS_RX_ERROR_SEQUENCE);
        final_reply = true;
        return;
    }
    uint16_t n = isotp_len - isotp_received;
    if (n > 7) {
        n = 7;
    }
    if (frame->dlc < n + 1) {
        rx_error(frame->timestamp_us, UDS_RX_ERROR_SEQUENCE);
        final_reply = true;
        return;
    }
    memcpy(&isotp_data[isotp_received], &frame->data[1], n);
    isotp_received += n;
    isotp_next_sn = (isotp_next_sn + 1) & 0x0F;
    // N_Cr: the ECU has up to a second between consecutive frames.
    window_end_us = frame->timestamp_us + REPLY_WINDOW_US;
    if (isotp_received == isotp_len) {
        message_complete(frame->timestamp_us);
        final_reply = true;
    }
}

esp_err_t uds_client_init(twai_node_handle_t twai_node) {
    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (!allowed(&requests[i])) {
            ESP_LOGE(TAG, "request %s is not on the allowlist", requests[i].name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    node = twai_node;
    return ESP_OK;
}

void uds_client_on_frame(const can_log_frame_t *frame) {
    if (state == STATE_IDLE || frame->extended || frame->remote || frame->dlc == 0) {
        return;
    }
    if (frame->id < current->rx_id_min || frame->id > current->rx_id_max) {
        return;
    }
    // Consecutive frames are not logged one by one (a 216-byte reply has 31); the complete message is.
    uint8_t type = frame->data[0] >> 4;
    if (type == ISOTP_CONSECUTIVE) {
        on_consecutive_frame(frame);
        return;
    }
    if (type != ISOTP_SINGLE && type != ISOTP_FIRST) {
        return;
    }
    replies++;
    add_log(UDS_LOG_RX, frame->timestamp_us / 1000, frame->id, frame->dlc, frame->data);
    log_frame_event(frame->timestamp_us, "uds_rx", frame->id, frame->dlc, frame->data);
    if (type == ISOTP_FIRST) {
        on_first_frame(frame);
        return;
    }
    // A physical request has one answer, unless the ECU says it needs more time. Functional ones wait for everybody.
    bool pending_reply = frame->dlc >= 4 && frame->data[1] == UDS_NRC && frame->data[3] == UDS_NRC_RESPONSE_PENDING;
    if (current->rx_id_min == current->rx_id_max && !pending_reply) {
        final_reply = true;
    }
}

void uds_client_poll(int64_t now_us, bool bus_ok) {
    switch (state) {
    case STATE_IDLE: {
        int index = atomic_exchange(&pending, -1);
        if (index < 0) {
            return;
        }
        atomic_store(&busy, true);
        current = &requests[index];
        replies = 0;
        final_reply = false;
        isotp_active = false;
        flow_control_due = false;
        // An error-passive or bus-off node has been disturbing the bus already; don't add to it.
        if (!bus_ok || !allowed(current)) {
            add_log(UDS_LOG_REFUSED, now_us / 1000, current->tx_id, 0, NULL);
            log_text_event(now_us, "uds_refused", current->name, 0);
            finish();
            return;
        }
        start_request(now_us);
        return;
    }
    case STATE_SENDING: {
        int result = atomic_load(&tx_result);
        if (result == TX_RESULT_OK) {
            state = STATE_WAITING;
        } else if (result == TX_RESULT_FAILED || now_us >= window_end_us) {
            if (attempts < MAX_ATTEMPTS) {
                state = STATE_BACKOFF;
                retry_at_us = now_us + RETRY_DELAY_US;
            } else {
                fail(now_us);
            }
        }
        return;
    }
    case STATE_BACKOFF:
        if (now_us < retry_at_us) {
            return;
        }
        if (bus_ok) {
            transmit(now_us);
        } else {
            fail(now_us);
        }
        return;
    case STATE_WAITING:
        if (flow_control_due && !final_reply) {
            flow_control_due = false;
            if (bus_ok) {
                start_flow_control(now_us);
            } else {
                fail(now_us);
            }
            return;
        }
        if (isotp_active && now_us >= window_end_us) {
            rx_error(now_us, UDS_RX_ERROR_INCOMPLETE);
        }
        if (final_reply || now_us >= window_end_us) {
            add_log(UDS_LOG_DONE, now_us / 1000, current->tx_id, replies, NULL);
            log_text_event(now_us, "uds_done", current->name, replies);
            finish();
        }
        return;
    }
}

void IRAM_ATTR uds_client_on_tx_done_from_isr(bool success) {
    atomic_store(&tx_result, success ? TX_RESULT_OK : TX_RESULT_FAILED);
}

esp_err_t uds_client_request(const char *name) {
    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (strcmp(requests[i].name, name) != 0) {
            continue;
        }
        int expected = -1;
        if (atomic_load(&busy) || !atomic_compare_exchange_strong(&pending, &expected, (int) i)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t uds_client_request_count(void) {
    return REQUEST_COUNT;
}

const uds_request_t *uds_client_get_request(size_t index) {
    return index < REQUEST_COUNT ? &requests[index] : NULL;
}

void uds_client_get_message(uds_message_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = message;
    taskEXIT_CRITICAL(&lock);
}

size_t uds_client_get_log(uds_log_entry_t *out, size_t max, uint32_t *seq) {
    size_t n = 0;
    taskENTER_CRITICAL(&lock);
    for (; n < log_count && n < max; n++) {
        out[n] = log_entries[(log_head + n) % UDS_CLIENT_LOG_LEN];
    }
    *seq = log_seq;
    taskEXIT_CRITICAL(&lock);
    return n;
}
