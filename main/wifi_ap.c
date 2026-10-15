#include "wifi_ap.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

#define WPA2_MIN_PASSWORD_LEN 8
#define HTTP_PORT 80

static const char *TAG = "wifi_ap";

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_WRONG_PASSWORD) {
        // A failed WPA2 handshake never produces STACONNECTED, so without this a bad password is silent.
        wifi_event_ap_wrong_password_t *event = data;
        ESP_LOGW(TAG, "station " MACSTR " used a wrong password", MAC2STR(event->mac));
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "station " MACSTR " joined", MAC2STR(event->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "station " MACSTR " left, reason %d", MAC2STR(event->mac), event->reason);
    }
}

// The Wi-Fi driver keeps calibration data and its config in NVS, so NVS must be up first.
static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition is full or was written by a newer NVS format. It holds nothing we need: wipe it.
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static esp_err_t start_ap(void) {
    const char *password = CONFIG_LIVE_WIFI_PASSWORD;
    size_t password_len = strlen(password);

    // The driver would reject a short WPA2 passphrase anyway, but with a less obvious error.
    ESP_RETURN_ON_FALSE(password_len == 0 || password_len >= WPA2_MIN_PASSWORD_LEN, ESP_ERR_INVALID_ARG, TAG,
        "password must be empty (open network) or at least %d characters", WPA2_MIN_PASSWORD_LEN);

    // Creates the AP network interface with a DHCP server on 192.168.4.1/24.
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL, ESP_FAIL, TAG, "AP netif");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
        TAG, "event handler");

    wifi_config_t cfg = {
        .ap = {
            .channel = CONFIG_LIVE_WIFI_CHANNEL,
            .max_connection = CONFIG_LIVE_WIFI_MAX_CONNECTIONS,
            .authmode = password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
            // Left at 0 the driver offers TKIP+CCMP; TKIP is deprecated and clients like iOS treat it as weak.
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
        },
    };
    strlcpy((char *) cfg.ap.ssid, CONFIG_LIVE_WIFI_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen((char *) cfg.ap.ssid);
    strlcpy((char *) cfg.ap.password, password, sizeof(cfg.ap.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    if (password_len == 0) {
        ESP_LOGW(TAG, "no password set: network \"%s\" is OPEN", CONFIG_LIVE_WIFI_SSID);
    }
    ESP_LOGI(TAG, "AP \"%s\" up on channel %d", CONFIG_LIVE_WIFI_SSID, CONFIG_LIVE_WIFI_CHANNEL);
    return ESP_OK;
}

// Convenience only: Android resolves .local names unreliably, so 192.168.4.1 always works too.
static esp_err_t start_mdns(void) {
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    esp_err_t err = mdns_hostname_set(CONFIG_LIVE_WIFI_MDNS_HOSTNAME);
    if (err == ESP_OK) {
        err = mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    }
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    ESP_LOGI(TAG, "reachable as %s.local", CONFIG_LIVE_WIFI_MDNS_HOSTNAME);
    return ESP_OK;
}

esp_err_t wifi_ap_start(void) {
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(start_ap(), TAG, "access point");

    esp_err_t err = start_mdns();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable (%s); use 192.168.4.1", esp_err_to_name(err));
    }
    return ESP_OK;
}
