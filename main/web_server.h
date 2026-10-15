#pragma once

#include "esp_err.h"

// Serves the live view: GET / returns the page (main/web/index.html), /ws streams the frame table and the logger
// status, and takes the page's commands (recording switch, marks). Protocol in docs/live-view.md.
// Requires wifi_ap_start() and live_data_init().
esp_err_t web_server_start(void);
