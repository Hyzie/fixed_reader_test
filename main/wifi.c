#include "wifi.h"
#include "wifi_config.h"
#include <stdio.h>
#include <string.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "WIFI";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static const int WIFI_MAXIMUM_RETRY = 5;
static char s_connected_ssid[64] = "";
static char s_ip_address[16] = "";
static bool s_wifi_initialized = false;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to WiFi (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "Failed to connect to WiFi after %d attempts", WIFI_MAXIMUM_RETRY);
        }
        // Clear connection info on disconnect
        s_connected_ssid[0] = '\0';
        s_ip_address[0] = '\0';
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Store IP address
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{
    if (s_wifi_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    // Create WiFi station netif (note: esp_netif_init() already called by Ethernet)
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // Optimize power management for stable connection
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // Disable power saving for stability
    
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi module initialized (STA mode, power save disabled)");

    // Try to connect with saved credentials
    char ssid[64] = "";
    char pass[64] = "";
    if (wifi_config_load(ssid, sizeof(ssid), pass, sizeof(pass)) == 0 && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Found saved WiFi credentials, attempting connection...");
        wifi_connect_with_credentials(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved WiFi credentials found. Configure via web interface.");
    }
}

void wifi_connect_with_credentials(const char* ssid, const char* password)
{
    if (!s_wifi_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return;
    }

    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
    
    // Reset event group
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        ESP_LOGE(TAG, "NVS not enough space for WiFi config. Please erase flash and reflash.");
        return;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return;
    }
    
    // Check WiFi state before attempting to connect
    wifi_ap_record_t ap_info;
    esp_err_t connection_status = esp_wifi_sta_get_ap_info(&ap_info);
    bool already_connected = (connection_status == ESP_OK);
    
    if (already_connected) {
        ESP_LOGI(TAG, "Already connected to WiFi, disconnecting first...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for disconnect
    }
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(ret));
        return;
    }

    // Store the SSID we're trying to connect to
    strncpy(s_connected_ssid, ssid, sizeof(s_connected_ssid) - 1);
}

void wifi_disconnect(void)
{
    if (!s_wifi_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Disconnecting from WiFi");
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "WiFi disconnect failed: %s", esp_err_to_name(ret));
    }
    
    // Clear connection info
    s_connected_ssid[0] = '\0';
    s_ip_address[0] = '\0';
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
}

bool wifi_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

const char* wifi_get_status(void)
{
    if (wifi_is_connected()) {
        return "connected";
    } else if (!s_wifi_initialized) {
        return "not_initialized";
    } else {
        return "disconnected";
    }
}

const char* wifi_get_connected_ssid(void)
{
    return s_connected_ssid;
}

const char* wifi_get_ip_address(void)
{
    return s_ip_address;
}

bool wifi_test_connection(const char* ssid, const char* password)
{
    if (!ssid || !password) {
        ESP_LOGE(TAG, "Invalid SSID or password for test");
        return false;
    }

    ESP_LOGI(TAG, "Testing WiFi connection to: %s", ssid);
    
    // Simple test: create a temporary WiFi config and test it
    wifi_config_t test_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strncpy((char*)test_config.sta.ssid, ssid, sizeof(test_config.sta.ssid) - 1);
    strncpy((char*)test_config.sta.password, password, sizeof(test_config.sta.password) - 1);

    // Stop current WiFi if running
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Set test config
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &test_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set test WiFi config: %s", esp_err_to_name(ret));
        esp_wifi_start(); // Restart WiFi
        return false;
    }

    // Start WiFi and try to connect
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Clear event bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start test WiFi connection: %s", esp_err_to_name(ret));
        return false;
    }

    // Wait for connection result (max 15 seconds)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    bool test_success = false;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Test connection successful to: %s", ssid);
        test_success = true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Test connection failed to: %s", ssid);
        test_success = false;
    } else {
        ESP_LOGW(TAG, "Test connection timeout to: %s", ssid);
        test_success = false;
    }

    // Always disconnect after test
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(2000)); // Increased delay for proper disconnect

    // Try to restore previous WiFi settings
    char saved_ssid[64], saved_pass[64];
    wifi_config_load(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
    if (strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Restoring connection to saved WiFi: %s", saved_ssid);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Additional delay before reconnecting
        wifi_connect_with_credentials(saved_ssid, saved_pass);
    }

    ESP_LOGI(TAG, "WiFi test completed. Result: %s", test_success ? "SUCCESS" : "FAILED");
    return test_success;
}