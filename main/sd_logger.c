#include "sd_logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
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

// Large stdio buffer so FATFS gets few, big writes instead of one per frame.
#define CAN_FILE_BUFFER_SIZE (16 * 1024)

static const char *TAG = "sd_logger";

static sdmmc_card_t *card;
static FILE *can_file;
static char *can_file_buffer;
static FILE *events_file;
static char session_dir[64];

static bool initialized;
static bool bus_initialized;

static void close_files(void) {
    if (can_file != NULL) {
        fclose(can_file);
        can_file = NULL;
    }

    // Must outlive can_file, which uses it as its stdio buffer.
    free(can_file_buffer);
    can_file_buffer = NULL;

    if (events_file != NULL) {
        fclose(events_file);
        events_file = NULL;
    }
}

static void cleanup(void) {
    close_files();

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

    char path[96];

    snprintf(path, sizeof(path), "%s/can.log", session_dir);
    can_file = fopen(path, "w");
    if (can_file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    can_file_buffer = malloc(CAN_FILE_BUFFER_SIZE);
    if (can_file_buffer == NULL || setvbuf(can_file, can_file_buffer, _IOFBF, CAN_FILE_BUFFER_SIZE) != 0) {
        ESP_LOGE(TAG, "Cannot set up write buffer");
        return ESP_ERR_NO_MEM;
    }

    snprintf(path, sizeof(path), "%s/events.csv", session_dir);
    events_file = fopen(path, "w");
    if (events_file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }

    if (fprintf(events_file, "timestamp_us,event,value\n") < 0) {
        ESP_LOGE(TAG, "Failed to write CSV header");
        return ESP_FAIL;
    }

    return ESP_OK;
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

    int written = fprintf(can_file, "(%" PRIu64 ".%06" PRIu64 ") can0 ",
        frame->timestamp_us / 1000000, frame->timestamp_us % 1000000);
    if (written < 0) {
        return ESP_FAIL;
    }

    written = frame->extended
        ? fprintf(can_file, "%08" PRIX32 "#", frame->id)
        : fprintf(can_file, "%03" PRIX32 "#", frame->id);
    if (written < 0) {
        return ESP_FAIL;
    }

    if (frame->remote) {
        // candump notation: "R" plus the requested length, if any.
        written = frame->dlc > 0 ? fprintf(can_file, "R%u\n", frame->dlc) : fprintf(can_file, "R\n");
        return written < 0 ? ESP_FAIL : ESP_OK;
    }

    // Classic CAN allows DLC 9..15, but they all mean 8 data bytes.
    unsigned len = frame->dlc > 8 ? 8 : frame->dlc;
    for (unsigned i = 0; i < len; i++) {
        if (fprintf(can_file, "%02X", frame->data[i]) < 0) {
            return ESP_FAIL;
        }
    }

    return fputc('\n', can_file) == EOF ? ESP_FAIL : ESP_OK;
}

esp_err_t sd_logger_log_event(uint64_t timestamp_us, const char *event, const char *value) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (fprintf(events_file, "%" PRIu64 ",%s,%s\n", timestamp_us, event, value) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_logger_flush(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (fflush(can_file) != 0 || fflush(events_file) != 0) {
        return ESP_FAIL;
    }

    // fflush only hands data to FATFS; fsync commits it and the directory entry (file size) to the card.
    if (fsync(fileno(can_file)) != 0 || fsync(fileno(events_file)) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sd_logger_stop(void) {
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = sd_logger_flush();
    cleanup();

    return err;
}
