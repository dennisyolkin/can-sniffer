#include "status_led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define STATUS_LED_GPIO       GPIO_NUM_4
#define STATUS_LED_COUNT      1
#define STATUS_LED_RMT_HZ     (10 * 1000 * 1000)

// Valid only after status_led_init() returned ESP_OK; all other functions rely on that.
static led_strip_handle_t led_strip;
static TimerHandle_t off_timer;
static volatile bool priority_active;

static esp_err_t status_led_write(status_led_color_t color) {
    esp_err_t err = led_strip_set_pixel(led_strip, 0, color.r, color.g, color.b);
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_refresh(led_strip);
}

static TickType_t ms_to_ticks(uint32_t duration_ms) {
    TickType_t ticks = pdMS_TO_TICKS(duration_ms);
    return ticks == 0 ? 1 : ticks;
}

static void off_timer_callback(TimerHandle_t timer) {
    (void) timer;
    priority_active = false;
    status_led_write(STATUS_LED_OFF);
}

esp_err_t status_led_init(void) {
    if (led_strip != NULL) {
        return ESP_OK;
    }

    const led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STATUS_LED_RMT_HZ,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(
        &strip_config,
        &rmt_config,
        &led_strip
    );
    if (err != ESP_OK) {
        return err;
    }

    off_timer = xTimerCreate(
        "led_off",
        1,              // Initial period; replaced when started
        pdFALSE,        // One-shot timer
        NULL,
        off_timer_callback
    );

    if (off_timer == NULL) {
        led_strip_del(led_strip);
        led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }
    priority_active = false;
    return ESP_OK;
}

esp_err_t status_led_set(status_led_color_t color) {
    if (priority_active) {
        return ESP_ERR_INVALID_STATE;
    }
    xTimerStop(off_timer, 0);
    return status_led_write(color);
}

esp_err_t status_led_show_for(status_led_color_t color, uint32_t duration_ms, bool priority) {
    if (duration_ms == 0) {
        return status_led_off();
    }
    if (priority_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = status_led_write(color);
    if (err != ESP_OK) {
        return err;
    }

    priority_active = priority;
    if (xTimerChangePeriod(off_timer, ms_to_ticks(duration_ms), 0) != pdPASS) {
        priority_active = false;
        status_led_write(STATUS_LED_OFF);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t status_led_show_for_block(status_led_color_t color, uint32_t duration_ms) {
    esp_err_t err = status_led_set(color);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(ms_to_ticks(duration_ms));
    return status_led_off();
}

esp_err_t status_led_off(void) {
    return status_led_set(STATUS_LED_OFF);
}
