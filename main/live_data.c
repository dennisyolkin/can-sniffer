#include "live_data.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_timer.h"

// 87 IDs on the car's bus plus 32 + 8 pages for the muxed ones, with room for IDs that only show up sometimes.
#define MAX_SLOTS 192
#define NO_SLOT 0xFF
#define STANDARD_ID_COUNT 0x800
// Marks are taken once per second; a few in a row from the page must fit.
#define MARK_QUEUE_LEN 8

_Static_assert(MAX_SLOTS <= NO_SLOT, "slot numbers are uint8_t");

// IDs that carry a page number in b0, so one entry per page is kept (see docs/signals.md).
typedef struct {
    uint16_t id;
    uint8_t shift;  // page = b0 >> shift
} muxed_id_t;

static const muxed_id_t muxed_ids[] = {
    {0x334, 3},  // Cell voltages, 4 per page, 27 pages.
    {0x3F4, 5},  // Battery temperature probes, 7 per page, 8 pages.
};

// The logger loop is the only writer of slots and slot_of; the lock keeps the web server's snapshot consistent.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static live_frame_t slots[MAX_SLOTS];
static unsigned slot_count;
static uint8_t slot_of[STANDARD_ID_COUNT];

static live_status_t status;
static int recording_request = -1;  // -1: none, otherwise the requested state.
static QueueHandle_t marks;

static const muxed_id_t *find_muxed(uint16_t id) {
    for (size_t i = 0; i < sizeof(muxed_ids) / sizeof(muxed_ids[0]); i++) {
        if (muxed_ids[i].id == id) {
            return &muxed_ids[i];
        }
    }
    return NULL;
}

// A muxed ID gets one slot per possible page, so the page is an offset from its first slot.
static uint8_t allocate(uint16_t id, const muxed_id_t *mux) {
    unsigned pages = mux != NULL ? 256u >> mux->shift : 1;
    if (slot_count + pages > MAX_SLOTS) {
        return NO_SLOT;
    }

    uint8_t base = slot_count;
    taskENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < pages; i++) {
        slots[base + i] = (live_frame_t) {
            .id = id,
            .page = mux != NULL ? i : LIVE_DATA_NO_PAGE,
        };
    }
    slot_count += pages;
    taskEXIT_CRITICAL(&lock);

    slot_of[id] = base;
    return base;
}

esp_err_t live_data_init(void) {
    memset(slot_of, NO_SLOT, sizeof(slot_of));
    marks = xQueueCreate(MARK_QUEUE_LEN, sizeof(live_mark_t));
    return marks != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void live_data_update(const can_log_frame_t *frame) {
    // None on this car's bus; they would need a different table.
    if (frame->extended) {
        return;
    }

    uint16_t id = frame->id & (STANDARD_ID_COUNT - 1);
    const muxed_id_t *mux = find_muxed(id);
    uint8_t slot = slot_of[id];
    if (slot == NO_SLOT) {
        slot = allocate(id, mux);
        if (slot == NO_SLOT) {
            return;
        }
    }
    if (mux != NULL && !frame->remote && frame->dlc > 0) {
        slot += frame->data[0] >> mux->shift;
    }

    live_frame_t *entry = &slots[slot];
    taskENTER_CRITICAL(&lock);
    entry->id = id | (frame->remote ? LIVE_DATA_REMOTE_FLAG : 0);
    entry->dlc = frame->dlc;
    entry->count++;
    entry->last_ms = (uint32_t) (frame->timestamp_us / 1000);
    memcpy(entry->data, frame->data, sizeof(entry->data));
    taskEXIT_CRITICAL(&lock);
}

void live_data_set_status(const live_status_t *new_status) {
    taskENTER_CRITICAL(&lock);
    status = *new_status;
    taskEXIT_CRITICAL(&lock);
}

bool live_data_take_recording_request(bool *on) {
    taskENTER_CRITICAL(&lock);
    int request = recording_request;
    recording_request = -1;
    taskEXIT_CRITICAL(&lock);

    if (request < 0) {
        return false;
    }
    *on = request == 1;
    return true;
}

bool live_data_take_mark(live_mark_t *mark) {
    return xQueueReceive(marks, mark, 0) == pdTRUE;
}

size_t live_data_snapshot(live_frame_t *out, size_t max) {
    size_t n = 0;
    taskENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < slot_count && n < max; i++) {
        // Pages of a muxed ID that were never sent.
        if (slots[i].count > 0) {
            out[n++] = slots[i];
        }
    }
    taskEXIT_CRITICAL(&lock);
    return n;
}

void live_data_get_status(live_status_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = status;
    taskEXIT_CRITICAL(&lock);
}

void live_data_request_recording(bool on) {
    taskENTER_CRITICAL(&lock);
    recording_request = on ? 1 : 0;
    taskEXIT_CRITICAL(&lock);
}

esp_err_t live_data_add_mark(const char *label) {
    live_mark_t mark = {.timestamp_us = esp_timer_get_time()};
    // The label ends up as a CSV field, so nothing that could split it.
    size_t i = 0;
    for (; label[i] != '\0' && i < sizeof(mark.label) - 1; i++) {
        char c = label[i];
        mark.label[i] = (c == ',' || c == '"' || (unsigned char) c < 0x20 || c == 0x7F) ? ' ' : c;
    }
    mark.label[i] = '\0';
    return xQueueSend(marks, &mark, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
