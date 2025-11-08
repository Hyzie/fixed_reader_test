#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uart.h"
#include "eth.h"
#include "wifi.h"
#include "mqtt_config.h"
#include "web.h"
#include "rfid.h"


static const char *TAG = "MAIN";

// Forward declaration
static void mqtt_task(void *pvParameters);

// MQTT task to handle connectivity and periodic batch publishing
static void mqtt_task(void *pvParameters)
{
    uint32_t last_batch_publish = 0;
    uint32_t last_connection_attempt = 0;
    const uint32_t BATCH_PUBLISH_INTERVAL_MS = 5000; // Publish batch every 5 seconds (reduced frequency)
    const uint32_t CONNECTION_RETRY_INTERVAL_MS = 10000; // Wait 10 seconds between connection attempts
    
    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Check if Ethernet is connected and MQTT should connect (with retry delay)
        if (eth_is_connected() && !mqtt_is_connected() && !mqtt_is_connecting()) {
            if ((now - last_connection_attempt) >= CONNECTION_RETRY_INTERVAL_MS) {
                ESP_LOGI(TAG, "Ethernet connected, attempting MQTT connection...");
                mqtt_connect();
                last_connection_attempt = now;
            }
        }
        
        // Periodic batch publishing for MQTT data (only if connection is stable)
        if (mqtt_is_connected() && (now - last_batch_publish) >= BATCH_PUBLISH_INTERVAL_MS) {
            if (rfid_get_mqtt_status_bool()) {
                mqtt_publish_periodic_batch();
            }
            // Also flush any remaining buffered data periodically
            mqtt_flush_buffer();
            last_batch_publish = now;
        }
        
        // Run connection health monitoring
        mqtt_connection_monitor();
        
        vTaskDelay(pdMS_TO_TICKS(2000)); // Check every 2 seconds (reduced frequency)
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize modules
    rfid_init();
    
    // Allow system to stabilize before starting UART task
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uart_start_rx_task();
    
    // Initialize Ethernet (for both web server and MQTT communication)
    eth_init();
    
    // Initialize WiFi (optional - can be disabled if only using Ethernet)
    // wifi_init();
    
    // Initialize MQTT client
    mqtt_init();

    // Start web server on Ethernet
    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }
    ESP_LOGI(TAG, "Web server started on Ethernet");
    
    // Start a task to handle MQTT connectivity and batch publishing (larger stack for JSON buffers)
    xTaskCreate(mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "System initialized successfully - Ethernet mode (Web Server + MQTT)");
}
