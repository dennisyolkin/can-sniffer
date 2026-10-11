#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} status_led_color_t;

#define STATUS_LED_OFF     ((status_led_color_t){0, 0, 0})
#define STATUS_LED_RED     ((status_led_color_t){255, 0, 0})
#define STATUS_LED_GREEN   ((status_led_color_t){0, 255, 0})
#define STATUS_LED_BLUE    ((status_led_color_t){0, 0, 255})
#define STATUS_LED_YELLOW  ((status_led_color_t){255, 255, 0})
#define STATUS_LED_WHITE   ((status_led_color_t){255, 255, 255})

esp_err_t status_led_init(void);
esp_err_t status_led_set(status_led_color_t color);
esp_err_t status_led_show_for(status_led_color_t color, uint32_t duration_ms, bool priority);
esp_err_t status_led_show_for_block(status_led_color_t color, uint32_t duration_ms);
esp_err_t status_led_off(void);