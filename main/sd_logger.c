#include "sd_logger.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_SESSIONS_DIR SD_MOUNT_POINT "/can-sniff"

#define SD_SPI_HOST SPI2_HOST

#define SD_PIN_MISO GPIO_NUM_2
#define SD_PIN_MOSI GPIO_NUM_15
#define SD_PIN_SCLK GPIO_NUM_14
#define SD_PIN_CS   GPIO_NUM_13

#define SD_MAX_TRANSFER_SIZE 4096

// Text is collected in RAM chunks and written by a separate task, so a slow card never stalls
// the RX path. SD cards occasionally block a write or fsync for well over a second (trip1 saw up
// to 1.9 s); at ~60 KB/s of text that needs ~120 KB of buffering to ride out without losing frames.
#define CHUNK_SIZE       (8 * 1024)
#define CHUNK_COUNT_MAX  24
#define CHUNK_COUNT_MIN  4
// Heap kept free for FATFS, stdio and the rest of the firmware after the chunk pool is allocated.
#define HEAP_RESERVE     (48 * 1024)

// Longest can.log line: "(18446744073709.551615) can0 1FFFFFFF#0011223344556677\n" is 56 bytes.
#define CAN_LINE_MAX     64
#define EVENT_LINE_MAX   160

// fsync commits data and the file size to the card, bounding what a power cut can lose. While a
// backlog is being written it is postponed (it costs card time), but never longer than the max.
#define SYNC_PERIOD_US     (1000 * 1000)
#define SYNC_MAX_PERIOD_US (5 * 1000 * 1000)

#define WRITER_STACK_SIZE 4096
#define WRITER_PRIORITY   5
// The logger loop runs on core 0 (app_main); the writer gets core 1 to itself.
#define WRITER_CORE       1

static const char *TAG = "sd_logger";

typedef enum {
    LOG_FILE_CAN,
    LOG_FILE_EVENTS,
    LOG_FILE_COUNT,
} log_file_t;

typedef struct {
    log_file_t file;
    size_t len;
    char *data;
} chunk_t;

static sdmmc_card_t *card;
static int fds[LOG_FILE_COUNT] = {-1, -1};
static char session_dir[64];

// Chunks circulate between the logger (fills them) and the writer (writes and returns them).
// Both queues hold every chunk at most once, so sends never block or fail.
static chunk_t chunks[CHUNK_COUNT_MAX];
static unsigned chunk_count;
static QueueHandle_t free_queue;
static QueueHandle_t full_queue;

// Owned by the logger side.
static chunk_t *current[LOG_FILE_COUNT];
static uint32_t dropped_frames;
static uint32_t buffers_min_free;

// Set by the writer on the first failed write or fsync; it then discards everything.
static volatile bool write_failed;

// Updated by the writer, read and reset by sd_logger_get_stats.
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t write_max_us;
static uint32_t sync_max_us;

static bool initialized;
static bool bus_initialized;

static void free_chunks(void) {
    for (unsigned i = 0; i < chunk_count; i++) {
        heap_caps_free(chunks[i].data);
        chunks[i].data = NULL;
    }
    chunk_count = 0;

    if (free_queue != NULL) {
        vQueueDelete(free_queue);
        free_queue = NULL;
    }
    if (full_queue != NULL) {
        vQueueDelete(full_queue);
        full_queue = NULL;
    }
}

static void cleanup(void) {
    for (unsigned i = 0; i < LOG_FILE_COUNT; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }

    free_chunks();

    if (card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        card = NULL;
    }

    if (bus_initialized) {
        spi_bus_free(SD_SPI_HOST);
        bus_initialized = false;
    }

    initialized = false;
}

static esp_err_t open_file(log_file_t file, const char *name) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", session_dir, name);

    fds[file] = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fds[file] < 0) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, data, len);
        if (written <= 0) {
            return ESP_FAIL;
        }
        data += written;
        len -= written;
    }
    return ESP_OK;
}

static esp_err_t create_session(void) {
    if (mkdir(SD_SESSIONS_DIR, 0775) != 0 && access(SD_SESSIONS_DIR, F_OK) != 0) {
        ESP_LOGE(TAG, "Cannot create sessions directory");
        return ESP_FAIL;
    }

    for (int attempt = 0; attempt < 10; attempt++) {
        uint32_t id = esp_random();

        snprintf(session_dir, sizeof(session_dir), SD_SESSIONS_DIR "/%08" PRIx32, id);

        if (mkdir(session_dir, 0775) == 0) {
            ESP_LOGI(TAG, "Created session: %s", session_dir);
            break;
        }

        session_dir[0] = '\0';
    }

    if (session_dir[0] == '\0') {
        ESP_LOGE(TAG, "Failed to create unique session directory");
        return ESP_FAIL;
    }

    esp_err_t err = open_file(LOG_FILE_CAN, "can.log");
    if (err != ESP_OK) {
        return err;
    }
    err = open_file(LOG_FILE_EVENTS, "events.csv");
    if (err != ESP_OK) {
        return err;
    }

    static const char header[] = "timestamp_us,event,value\n";
    if (write_all(fds[LOG_FILE_EVENTS], header, sizeof(header) - 1) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write CSV header");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Takes as many chunks as the heap allows, up to the max. DMA-capable memory lets the SD driver
// send whole sectors straight from the chunk instead of copying through a bounce buffer.
static esp_err_t allocate_chunks(void) {
    free_queue = xQueueCreate(CHUNK_COUNT_MAX, sizeof(chunk_t *));
    full_queue = xQueueCreate(CHUNK_COUNT_MAX, sizeof(chunk_t *));
    if (free_queue == NULL || full_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    while (chunk_count < CHUNK_COUNT_MAX
        && heap_caps_get_free_size(MALLOC_CAP_DMA) >= HEAP_RESERVE + CHUNK_SIZE) {
        chunk_t *chunk = &chunks[chunk_count];
        chunk->data = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA);
        if (chunk->data == NULL) {
            break;
        }
        chunk_count++;
        xQueueSend(free_queue, &chunk, 0);
    }

    if (chunk_count < CHUNK_COUNT_MIN) {
        ESP_LOGE(TAG, "Only %u write buffers fit in the heap", chunk_count);
        return ESP_ERR_NO_MEM;
    }

    buffers_min_free = chunk_count;
    ESP_LOGI(TAG, "%u write buffers of %d bytes", chunk_count, CHUNK_SIZE);
    return ESP_OK;
}

static void record_max(uint32_t *max, int64_t start_us) {
    uint32_t elapsed = (uint32_t) (esp_timer_get_time() - start_us);
    taskENTER_CRITICAL(&stats_lock);
    if (elapsed > *max) {
        *max = elapsed;
    }
    taskEXIT_CRITICAL(&stats_lock);
}

static void sync_files(bool dirty[LOG_FILE_COUNT]) {
    for (unsigned i = 0; i < LOG_FILE_COUNT; i++) {
        if (!dirty[i] || write_failed) {
            continue;
        }
        int64_t start = esp_timer_get_time();
        if (fsync(fds[i]) != 0) {
            write_failed = true;
        }
        record_max(&sync_max_us, start);
        dirty[i] = false;
    }
}

// The only code touching the card after init, so FATFS calls never block the logger.
static void writer_task(void *arg) {
    bool dirty[LOG_FILE_COUNT] = {false};
    int64_t last_sync = esp_timer_get_time();

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (dirty[LOG_FILE_CAN] || dirty[LOG_FILE_EVENTS]) {
            int64_t left_us = last_sync + SYNC_PERIOD_US - esp_timer_get_time();
            wait = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) + 1 : 0;
        }

        chunk_t *chunk;
        if (xQueueReceive(full_queue, &chunk, wait) == pdTRUE) {
            if (!write_failed) {
                int64_t start = esp_timer_get_time();
                if (write_all(fds[chunk->file], chunk->data, chunk->len) != ESP_OK) {
                    write_failed = true;
                }
                record_max(&write_max_us, start);
                dirty[chunk->file] = true;
            }
            chunk->len = 0;
            xQueueSend(free_queue, &chunk, 0);
        }

        int64_t since_sync = esp_timer_get_time() - last_sync;
        bool backlog = uxQueueMessagesWaiting(full_queue) > 0;
        if (since_sync >= SYNC_MAX_PERIOD_US || (since_sync >= SYNC_PERIOD_US && !backlog)) {
            sync_files(dirty);
            last_sync = esp_timer_get_time();
        }
    }
}

static void hand_over(log_file_t file) {
    chunk_t *chunk = current[file];
    if (chunk != NULL && chunk->len > 0) {
        xQueueSend(full_queue, &chunk, 0);
        current[file] = NULL;
    }
}

// Returns room for `len` more bytes in the file's current chunk, starting a new chunk if needed.
// Never blocks: NULL means every chunk is waiting for the card.
static char *reserve(log_file_t file, size_t len) {
    chunk_t *chunk = current[file];
    if (chunk != NULL && CHUNK_SIZE - chunk->len >= len) {
        return chunk->data + chunk->len;
    }

    hand_over(file);
    if (current[file] == NULL) {
        if (xQueueReceive(free_queue, &chunk, 0) != pdTRUE) {
            return NULL;
        }
        chunk->file = file;
        chunk->len = 0;
        current[file] = chunk;

        uint32_t free_now = uxQueueMessagesWaiting(free_queue);
        if (free_now < buffers_min_free) {
            buffers_min_free = free_now;
        }
    }
    return current[file]->data + current[file]->len;
}

static char *put_hex(char *p, uint32_t value, unsigned digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = digits; i > 0; i--) {
        p[i - 1] = hex[value & 0xF];
        value >>= 4;
    }
    return p + digits;
}

// Zero-padded to `min_digits`.
static char *put_dec(char *p, uint64_t value, unsigned min_digits) {
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = '0' + value % 10;
        value /= 10;
    } while (value > 0);
    while (n < min_digits) {
        digits[n++] = '0';
    }
    while (n > 0) {
        *p++ = digits[--n];
    }
    return p;
}

esp_err_t sd_logger_init(void) {
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t err;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    spi_bus_config_t bus_config = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_MAX_TRANSFER_SIZE,
    };

    err = spi_bus_initialize(SD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    bus_initialized = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SD_SPI_HOST;
    slot_config.gpio_cs = SD_PIN_CS;
    // Before every command the driver waits (default up to 40 ms) for MISO to read high with CS
    // deasserted. MISO is GPIO 2 with no pull-up, so it reads low and every command paid the
    // full timeout: trip2 saw ~375 ms per 8 KB write (~20 KB/s). Busy is still polled after
    // each write with CS asserted, which is where the card actually signals it.
    slot_config.wait_for_miso = -1;

    err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        cleanup();
        return err;
    }

    err = create_session();
    if (err == ESP_OK) {
        err = allocate_chunks();
    }
    if (err == ESP_OK
        && xTaskCreatePinnedToCore(writer_task, "sd_writer", WRITER_STACK_SIZE, NULL, WRITER_PRIORITY, NULL,
            WRITER_CORE) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    initialized = true;

    ESP_LOGI(TAG, "SDcard logger initialized");
    return ESP_OK;
}

esp_err_t sd_logger_log_can(const can_log_frame_t *frame) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (write_failed) {
        return ESP_FAIL;
    }

    char *start = reserve(LOG_FILE_CAN, CAN_LINE_MAX);
    if (start == NULL) {
        dropped_frames++;
        return ESP_ERR_NO_MEM;
    }

    // Hand-formatted: this runs for every frame, and printf is several times slower.
    char *p = start;
    *p++ = '(';
    p = put_dec(p, frame->timestamp_us / 1000000, 1);
    *p++ = '.';
    p = put_dec(p, frame->timestamp_us % 1000000, 6);
    memcpy(p, ") can0 ", 7);
    p += 7;
    p = put_hex(p, frame->id, frame->extended ? 8 : 3);
    *p++ = '#';

    if (frame->remote) {
        // candump notation: "R" plus the requested length, if any.
        *p++ = 'R';
        if (frame->dlc > 0) {
            p = put_dec(p, frame->dlc, 1);
        }
    } else {
        // Classic CAN allows DLC 9..15, but they all mean 8 data bytes.
        unsigned len = frame->dlc > 8 ? 8 : frame->dlc;
        for (unsigned i = 0; i < len; i++) {
            p = put_hex(p, frame->data[i], 2);
        }
    }
    *p++ = '\n';

    current[LOG_FILE_CAN]->len += p - start;
    return ESP_OK;
}

esp_err_t sd_logger_log_event(uint64_t timestamp_us, const char *event, const char *value) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (write_failed) {
        return ESP_FAIL;
    }

    char line[EVENT_LINE_MAX];
    int len = snprintf(line, sizeof(line), "%" PRIu64 ",%s,%s\n", timestamp_us, event, value);
    if (len < 0 || len >= (int) sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *dest = reserve(LOG_FILE_EVENTS, len);
    if (dest == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(dest, line, len);
    current[LOG_FILE_EVENTS]->len += len;
    return ESP_OK;
}

esp_err_t sd_logger_flush(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    hand_over(LOG_FILE_CAN);
    hand_over(LOG_FILE_EVENTS);
    return write_failed ? ESP_FAIL : ESP_OK;
}

esp_err_t sd_logger_get_stats(sd_logger_stats_t *stats) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&stats_lock);
    stats->write_max_us = write_max_us;
    stats->sync_max_us = sync_max_us;
    write_max_us = 0;
    sync_max_us = 0;
    taskEXIT_CRITICAL(&stats_lock);

    stats->dropped_frames = dropped_frames;
    stats->buffers_min_free = buffers_min_free;
    stats->buffer_count = chunk_count;
    buffers_min_free = uxQueueMessagesWaiting(free_queue);
    return ESP_OK;
}
