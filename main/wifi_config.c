#include "wifi_config.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "WIFI_CFG";
static const char *NVS_NAMESPACE = "wifi_cfg";

int wifi_config_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (ssid && strlen(ssid) > 0) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err != ESP_OK) {
            nvs_close(h);
            ESP_LOGE(TAG, "nvs_set_str ssid failed: %s", esp_err_to_name(err));
            return -2;
        }
    }
    if (pass) {
        err = nvs_set_str(h, "pass", pass);
        if (err != ESP_OK) {
            nvs_close(h);
            ESP_LOGE(TAG, "nvs_set_str pass failed: %s", esp_err_to_name(err));
            return -3;
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return -4;
    }
    ESP_LOGI(TAG, "Wi-Fi config saved (ssid='%s')", ssid ? ssid : "");
    return 0;
}

int wifi_config_load(char *ssid_buf, size_t ssid_len, char *pass_buf, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open read failed: %s", esp_err_to_name(err));
        return -1;
    }
    size_t required = ssid_len;
    err = nvs_get_str(h, "ssid", ssid_buf, &required);
    if (err != ESP_OK) {
        ssid_buf[0] = '\0';
    }
    required = pass_len;
    err = nvs_get_str(h, "pass", pass_buf, &required);
    if (err != ESP_OK) {
        pass_buf[0] = '\0';
    }
    nvs_close(h);
    return 0;
}
