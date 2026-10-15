#pragma once

#include "esp_err.h"

// Brings up NVS, the network stack and the Wi-Fi access point (Kconfig: menuconfig → Live view Wi-Fi), and
// announces the board as <hostname>.local over mDNS. The AP's own address is 192.168.4.1.
esp_err_t wifi_ap_start(void);
