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

static const char *TAG = "uds_client";

// IDs: PSA-RE diag/AEE2010.ev. They never appear in the car's own traffic (checked against trip3 and the test
// captures), so a request cannot collide with an ECU's frame. Padding is 0x00.
// Only ECUs that answered from the OBD port (tx-logs-2) are listed; the BSI, the cluster and OBD-II never did
// (docs/live-view.md, "Transmitting").
// BMS reads: DIDs from PSA-RE TBMU/BMU_CTE1 whose value is at most 4 bytes, so the reply (62 + DID + value) fits in
// one frame. The page decodes them; the label's "ECU:" prefix groups the buttons.
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
};
#define REQUEST_COUNT (sizeof(requests) / sizeof(requests[0]))

typedef enum {
    STATE_IDLE,
    STATE_SENDING,  // Waiting for on_tx_done.
    STATE_BACKOFF,  // An attempt failed; waiting before the next one.
    STATE_WAITING,  // Sent; collecting replies until the window closes.
} state_t;

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
// The driver keeps a pointer to the frame until it is sent.
static twai_frame_t tx_frame;
static uint8_t tx_data[8];

// Shared with the web server.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uds_log_entry_t log_entries[UDS_CLIENT_LOG_LEN];
static unsigned log_head;
static unsigned log_count;
static uint32_t log_seq;

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

static void add_log(uds_log_kind_t kind, uint32_t t_ms, uint16_t id, uint8_t len, const uint8_t *data) {
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

static void transmit(int64_t now_us) {
    attempts++;
    atomic_store(&tx_result, TX_RESULT_NONE);
    memcpy(tx_data, current->data, sizeof(tx_data));
    tx_frame = (twai_frame_t) {
        .header.id = current->tx_id,
        .buffer = tx_data,
        .buffer_len = sizeof(tx_data),
    };
    window_end_us = now_us + REPLY_WINDOW_US;
    if (twai_node_transmit(node, &tx_frame, 0) != ESP_OK) {
        atomic_store(&tx_result, TX_RESULT_FAILED);
    }
    state = STATE_SENDING;
    add_log(UDS_LOG_TX, now_us / 1000, current->tx_id, sizeof(tx_data), tx_data);
    log_frame_event(now_us, "uds_tx", current->tx_id, sizeof(tx_data), tx_data);
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
    if (state == STATE_IDLE || frame->extended || frame->remote) {
        return;
    }
    if (frame->id < current->rx_id_min || frame->id > current->rx_id_max) {
        return;
    }
    replies++;
    add_log(UDS_LOG_RX, frame->timestamp_us / 1000, frame->id, frame->dlc, frame->data);
    log_frame_event(frame->timestamp_us, "uds_rx", frame->id, frame->dlc, frame->data);
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
        attempts = 0;
        replies = 0;
        final_reply = false;
        // An error-passive or bus-off node has been disturbing the bus already; don't add to it.
        if (!bus_ok || !allowed(current)) {
            add_log(UDS_LOG_REFUSED, now_us / 1000, current->tx_id, 0, NULL);
            log_text_event(now_us, "uds_refused", current->name, 0);
            finish();
            return;
        }
        transmit(now_us);
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
