#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

#include "sd_logger.h"
#include "status_led.h"

#define CAN_TX_GPIO GPIO_NUM_27
#define CAN_RX_GPIO GPIO_NUM_26
// SN65HVD231 Rs pin: LOW selects high-speed mode, HIGH puts the transceiver into standby.
#define CAN_SE_GPIO GPIO_NUM_23
#define CAN_BITRATE 500000

// Only has to cover scheduling hiccups of the logger loop: SD writes happen in their own task.
#define RX_QUEUE_SIZE 256
#define RX_IDLE_SAMPLES 1000

#define HEARTBEAT_PERIOD_US (1000 * 1000)
#define HEARTBEAT_BLINK_MS  50

static const char *TAG = "can_sniffer";

typedef struct {
    twai_frame_t frame;
    uint8_t data[TWAI_FRAME_MAX_LEN];
} rx_slot_t;

// Single-producer (ISR) / single-consumer (logger loop) ring.
// rx_free counts empty slots, rx_ready counts filled ones.
static rx_slot_t rx_slots[RX_QUEUE_SIZE];
static SemaphoreHandle_t rx_free;
static SemaphoreHandle_t rx_ready;
static unsigned rx_write;
static unsigned rx_read;

static volatile uint32_t rx_dropped;
// Logged for diagnostics only. In listen-only mode the ESP32 driver forces REC to 128 (errata
// workaround against error frames), so the state is always error_passive and says nothing about
// bus health. Use bus_errors for that.
static volatile twai_error_state_t bus_state = TWAI_ERROR_ACTIVE;

static twai_node_handle_t can_node;

// Bus error counts by type, as reported by on_error. A listen-only node never transmits,
// so bit and ACK errors are not expected; form and stuff errors point at a corrupted signal.
typedef struct {
    uint32_t total;
    uint32_t form;
    uint32_t stuff;
    uint32_t bit;
    uint32_t ack;
} bus_error_counts_t;

static volatile bus_error_counts_t bus_errors;

static const char *const bus_state_names[] = {
    [TWAI_ERROR_ACTIVE] = "error_active",
    [TWAI_ERROR_WARNING] = "error_warning",
    [TWAI_ERROR_PASSIVE] = "error_passive",
    [TWAI_ERROR_BUS_OFF] = "bus_off",
};

static bool IRAM_ATTR on_rx_done(
    twai_node_handle_t node,
    const twai_rx_done_event_data_t *edata,
    void *user_ctx) {
    BaseType_t higher_priority_task_woken = pdFALSE;

    // A frame not received here is overwritten by the next one, so a full ring means a lost frame.
    if (xSemaphoreTakeFromISR(rx_free, &higher_priority_task_woken) != pdTRUE) {
        rx_dropped++;
        return false;
    }

    rx_slot_t *slot = &rx_slots[rx_write];

    if (twai_node_receive_from_isr(node, &slot->frame) == ESP_OK) {
        rx_write = (rx_write + 1) % RX_QUEUE_SIZE;
        xSemaphoreGiveFromISR(rx_ready, &higher_priority_task_woken);
    } else {
        rx_dropped++;
        xSemaphoreGiveFromISR(rx_free, &higher_priority_task_woken);
    }

    return higher_priority_task_woken == pdTRUE;
}

static bool IRAM_ATTR on_state_change(
    twai_node_handle_t node,
    const twai_state_change_event_data_t *edata,
    void *user_ctx) {
    bus_state = edata->new_sta;
    return false;
}

static bool IRAM_ATTR on_error(
    twai_node_handle_t node,
    const twai_error_event_data_t *edata,
    void *user_ctx) {
    bus_errors.total++;
    if (edata->err_flags.form_err) {
        bus_errors.form++;
    }
    if (edata->err_flags.stuff_err) {
        bus_errors.stuff++;
    }
    if (edata->err_flags.bit_err) {
        bus_errors.bit++;
    }
    if (edata->err_flags.ack_err) {
        bus_errors.ack++;
    }
    return false;
}

// Never returns. Leaves the LED solid red so the failure is visible without a laptop.
static void halt_with_error(const char *what, esp_err_t err) {
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    status_led_set(STATUS_LED_RED);
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

static esp_err_t transceiver_init(void) {
    // TX is held recessive as a plain GPIO and not given to the TWAI controller at all,
    // so the hardware cannot drive the bus whatever the driver does.
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << CAN_TX_GPIO) | (1ULL << CAN_SE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level(CAN_TX_GPIO, 1);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(CAN_SE_GPIO, 0);
}

// Diagnostic: an idle bus is recessive, so RX should read HIGH. Low samples mean a stuck
// or noisy bus, or an unpowered transceiver. Must run before TWAI takes over the RX pin.
static uint32_t sample_rx_low_count(void) {
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CAN_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    if (gpio_config(&config) != ESP_OK) {
        return UINT32_MAX;
    }

    uint32_t low = 0;
    for (unsigned i = 0; i < RX_IDLE_SAMPLES; i++) {
        if (gpio_get_level(CAN_RX_GPIO) == 0) {
            low++;
        }
        esp_rom_delay_us(10);
    }
    return low;
}

static esp_err_t twai_start(void) {
    rx_free = xSemaphoreCreateCounting(RX_QUEUE_SIZE, RX_QUEUE_SIZE);
    rx_ready = xSemaphoreCreateCounting(RX_QUEUE_SIZE, 0);
    if (rx_free == NULL || rx_ready == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (unsigned i = 0; i < RX_QUEUE_SIZE; i++) {
        rx_slots[i].frame.buffer = rx_slots[i].data;
        rx_slots[i].frame.buffer_len = sizeof(rx_slots[i].data);
    }

    twai_onchip_node_config_t config = {
        .io_cfg = {
            .tx = -1,
            .rx = CAN_RX_GPIO,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing.bitrate = CAN_BITRATE,
        // 1 MHz: frame timestamps are in microseconds since boot, same clock as esp_timer_get_time().
        .timestamp_resolution_hz = 1000000,
        .flags.enable_listen_only = true,
    };

    esp_err_t err = twai_new_node_onchip(&config, &can_node);
    if (err != ESP_OK) {
        return err;
    }

    twai_event_callbacks_t callbacks = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
    };

    err = twai_node_register_event_callbacks(can_node, &callbacks, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return twai_node_enable(can_node);
}

// ESP_ERR_NO_MEM means a line was dropped while the card was slow; sd_logger counts that. Anything else
// means the card is full or gone and logging has stopped.
static bool sd_alive(esp_err_t err) {
    return err == ESP_OK || err == ESP_ERR_NO_MEM;
}

static esp_err_t log_uint_event(uint64_t timestamp_us, const char *event, uint32_t value) {
    char text[12];
    snprintf(text, sizeof(text), "%" PRIu32, value);
    return sd_logger_log_event(timestamp_us, event, text);
}

// Logs the bus error counters if any error happened since the last call.
static esp_err_t log_bus_errors(uint64_t timestamp_us, uint32_t *total_reported) {
    // Copied field by field without locking: the ISR may update it mid-copy, which at worst
    // makes one line slightly inconsistent. Good enough for diagnostics.
    bus_error_counts_t errors = {
        .total = bus_errors.total,
        .form = bus_errors.form,
        .stuff = bus_errors.stuff,
        .bit = bus_errors.bit,
        .ack = bus_errors.ack,
    };
    if (errors.total == *total_reported) {
        return ESP_OK;
    }
    *total_reported = errors.total;

    char text[96];
    snprintf(text, sizeof(text), "total=%" PRIu32 " form=%" PRIu32 " stuff=%" PRIu32 " bit=%" PRIu32
        " ack=%" PRIu32,
        errors.total, errors.form, errors.stuff, errors.bit, errors.ack);
    return sd_logger_log_event(timestamp_us, "bus_errors", text);
}

// Write latencies show how long the card stalls; buffers_min_free how close that came to dropping frames.
static esp_err_t log_sd_stats(uint64_t timestamp_us, uint32_t *dropped_reported) {
    sd_logger_stats_t stats;
    esp_err_t err = sd_logger_get_stats(&stats);
    if (err != ESP_OK) {
        return err;
    }

    if (stats.dropped_frames != *dropped_reported) {
        *dropped_reported = stats.dropped_frames;
        err = log_uint_event(timestamp_us, "sd_dropped_total", stats.dropped_frames);
        if (!sd_alive(err)) {
            return err;
        }
    }

    char text[96];
    snprintf(text, sizeof(text), "write_max_us=%" PRIu32 " sync_max_us=%" PRIu32 " buffers_min_free=%" PRIu32
        "/%" PRIu32,
        stats.write_max_us, stats.sync_max_us, stats.buffers_min_free, stats.buffer_count);
    return sd_logger_log_event(timestamp_us, "sd_stats", text);
}

// One short blink per heartbeat; the color tells the state (see CLAUDE.md, "Status LED").
static void heartbeat_blink(bool sd_ok, uint32_t frames, uint32_t errors) {
    status_led_color_t color;
    if (!sd_ok) {
        color = STATUS_LED_RED;
    } else if (errors > 0) {
        color = STATUS_LED_YELLOW;
    } else if (frames > 0) {
        color = STATUS_LED_GREEN;
    } else {
        color = STATUS_LED_BLUE;
    }
    // Losing a blink is harmless, so the result is ignored.
    (void) status_led_show_for(color, HEARTBEAT_BLINK_MS, false);
}

void app_main(void) {
    ESP_ERROR_CHECK(status_led_init());

    esp_err_t err = sd_logger_init();
    if (err != ESP_OK) {
        halt_with_error("SD logger init", err);
    }

    err = transceiver_init();
    if (err != ESP_OK) {
        halt_with_error("CAN transceiver init", err);
    }

    // Give the transceiver time to leave standby before sampling.
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t rx_low = sample_rx_low_count();

    err = twai_start();
    if (err != ESP_OK) {
        halt_with_error("TWAI start", err);
    }

    ESP_LOGI(TAG, "Listening passively at %d bit/s", CAN_BITRATE);

    bool sd_ok = sd_alive(log_uint_event(esp_timer_get_time(), "bitrate", CAN_BITRATE))
        && sd_alive(log_uint_event(esp_timer_get_time(), "rx_idle_low_per_1000", rx_low));
    uint32_t frames_since_heartbeat = 0;
    uint32_t dropped_reported = 0;
    uint32_t bus_errors_reported = 0;
    uint32_t sd_dropped_reported = 0;
    uint32_t bus_errors_at_heartbeat = 0;
    twai_error_state_t state_reported = TWAI_ERROR_ACTIVE;
    int64_t next_heartbeat = esp_timer_get_time() + HEARTBEAT_PERIOD_US;

    while (true) {
        if (xSemaphoreTake(rx_ready, pdMS_TO_TICKS(100)) == pdTRUE) {
            const twai_frame_t *frame = &rx_slots[rx_read].frame;
            can_log_frame_t entry = {
                .timestamp_us = frame->header.timestamp,
                .id = frame->header.id,
                .dlc = frame->header.dlc,
                .extended = frame->header.ide,
                .remote = frame->header.rtr,
            };
            memcpy(entry.data, frame->buffer, sizeof(entry.data));

            // Free the slot before the (possibly slow) SD write.
            rx_read = (rx_read + 1) % RX_QUEUE_SIZE;
            xSemaphoreGive(rx_free);

            frames_since_heartbeat++;
            // After a write failure (card full or removed) stop writing; the LED reports it.
            sd_ok = sd_ok && sd_alive(sd_logger_log_can(&entry));
        }

        int64_t now = esp_timer_get_time();
        if (now < next_heartbeat) {
            continue;
        }
        next_heartbeat = now + HEARTBEAT_PERIOD_US;

        if (sd_ok) {
            uint32_t dropped = rx_dropped;
            if (dropped != dropped_reported) {
                sd_ok = sd_alive(log_uint_event(now, "rx_dropped_total", dropped));
                dropped_reported = dropped;
            }

            twai_error_state_t state = bus_state;
            if (sd_ok && state != state_reported) {
                sd_ok = sd_alive(sd_logger_log_event(now, "bus_state", bus_state_names[state]));
                state_reported = state;
            }

            sd_ok = sd_ok && sd_alive(log_bus_errors(now, &bus_errors_reported));
            sd_ok = sd_ok && sd_alive(log_sd_stats(now, &sd_dropped_reported));
            sd_ok = sd_ok && sd_logger_flush() == ESP_OK;
        }

        uint32_t bus_errors_total = bus_errors.total;
        heartbeat_blink(sd_ok, frames_since_heartbeat, bus_errors_total - bus_errors_at_heartbeat);
        bus_errors_at_heartbeat = bus_errors_total;
        frames_since_heartbeat = 0;
    }
}
