#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"   // ESP-IDF MQTT client
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"
#include "mqtt_config.h"   // Our local MQTT configuration
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MQTT";
static const char *NVS_NAMESPACE = "mqtt_cfg";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static mqtt_config_t s_mqtt_config = {0};
static bool s_mqtt_connected = false;
static bool s_mqtt_initialized = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        s_mqtt_connected = true;
        
        // Subscribe to command topic if configured
        if (strlen(s_mqtt_config.subscribe_topic) > 0) {
            int msg_id = esp_mqtt_client_subscribe(client, s_mqtt_config.subscribe_topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", s_mqtt_config.subscribe_topic, msg_id);
        }
        
        // Publish connection status
        mqtt_publish_status("online");
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT Unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT Published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT Data received");
        ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
        
        // TODO: Handle received commands here
        // You can add command processing logic based on the received data
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT Error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;

    default:
        ESP_LOGI(TAG, "Other MQTT event id:%d", event->event_id);
        break;
    }
}

void mqtt_init(void)
{
    if (s_mqtt_initialized) {
        ESP_LOGW(TAG, "MQTT already initialized");
        return;
    }

    // Initialize with default configuration
    memset(&s_mqtt_config, 0, sizeof(mqtt_config_t));
    strcpy(s_mqtt_config.broker_uri, "mqtts://9f9bbeafeb6a45d6b8dd97ca6951480d.s1.eu.hivemq.cloud:8883");
    strcpy(s_mqtt_config.client_id, "esp32_rfid_reader");
    strcpy(s_mqtt_config.username, "helloworld");
    strcpy(s_mqtt_config.password, "Hh1234567");
    strcpy(s_mqtt_config.publish_topic, "rfid/tags");
    strcpy(s_mqtt_config.subscribe_topic, "rfid/commands");

    // Try to load saved configuration (will override defaults if available)
    mqtt_load_config(&s_mqtt_config);
    
    s_mqtt_initialized = true;
    ESP_LOGI(TAG, "MQTT module initialized with broker: %s", s_mqtt_config.broker_uri);
}

void mqtt_set_config(const mqtt_config_t* config)
{
    if (config) {
        memcpy(&s_mqtt_config, config, sizeof(mqtt_config_t));
        ESP_LOGI(TAG, "MQTT config updated: broker=%s, client_id=%s", 
                 s_mqtt_config.broker_uri, s_mqtt_config.client_id);
    }
}

void mqtt_get_config(mqtt_config_t* config)
{
    if (config) {
        memcpy(config, &s_mqtt_config, sizeof(mqtt_config_t));
    }
}

int mqtt_save_config(const mqtt_config_t* config)
{
    if (!config) return -1;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    // Save all config fields
    nvs_set_str(h, "broker_uri", config->broker_uri);
    nvs_set_str(h, "client_id", config->client_id);
    nvs_set_str(h, "username", config->username);
    nvs_set_str(h, "password", config->password);
    nvs_set_str(h, "pub_topic", config->publish_topic);
    nvs_set_str(h, "sub_topic", config->subscribe_topic);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        // Update local config
        memcpy(&s_mqtt_config, config, sizeof(mqtt_config_t));
        ESP_LOGI(TAG, "MQTT config saved");
        return 0;
    } else {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return -1;
    }
}

int mqtt_load_config(mqtt_config_t* config)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open read failed: %s, using defaults", esp_err_to_name(err));
        return -1;
    }

    size_t required_size = 0;
    
    // Load all config fields (only override if they exist in NVS)
    required_size = sizeof(config->broker_uri);
    esp_err_t ret = nvs_get_str(h, "broker_uri", config->broker_uri, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load broker_uri: %s", esp_err_to_name(ret));
    }
    
    required_size = sizeof(config->client_id);
    ret = nvs_get_str(h, "client_id", config->client_id, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load client_id: %s", esp_err_to_name(ret));
    }
    
    required_size = sizeof(config->username);
    ret = nvs_get_str(h, "username", config->username, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load username: %s", esp_err_to_name(ret));
    }
    
    required_size = sizeof(config->password);
    ret = nvs_get_str(h, "password", config->password, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load password: %s", esp_err_to_name(ret));
    }
    
    required_size = sizeof(config->publish_topic);
    ret = nvs_get_str(h, "pub_topic", config->publish_topic, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load publish_topic: %s", esp_err_to_name(ret));
    }
    
    required_size = sizeof(config->subscribe_topic);
    ret = nvs_get_str(h, "sub_topic", config->subscribe_topic, &required_size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load subscribe_topic: %s", esp_err_to_name(ret));
    }

    nvs_close(h);
    
    ESP_LOGI(TAG, "MQTT config loaded: broker=%s", config->broker_uri);
    return 0;
}

void mqtt_connect(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, cannot start MQTT");
        return;
    }

    if (strlen(s_mqtt_config.broker_uri) == 0) {
        ESP_LOGW(TAG, "MQTT broker URI not configured");
        return;
    }

    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_config.broker_uri,
        .broker.verification.certificate = NULL,  // Use default CA certificates
        .broker.verification.skip_cert_common_name_check = false,
        .broker.verification.use_global_ca_store = false,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };

    // Set client ID
    if (strlen(s_mqtt_config.client_id) > 0) {
        mqtt_cfg.credentials.client_id = s_mqtt_config.client_id;
    }

    // Set username/password if configured
    if (strlen(s_mqtt_config.username) > 0) {
        mqtt_cfg.credentials.username = s_mqtt_config.username;
    }
    if (strlen(s_mqtt_config.password) > 0) {
        mqtt_cfg.credentials.authentication.password = s_mqtt_config.password;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client) {
        esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_mqtt_client);
        ESP_LOGI(TAG, "MQTT client started");
    } else {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
    }
}

void mqtt_disconnect(void)
{
    if (s_mqtt_client) {
        mqtt_publish_status("offline");
        esp_mqtt_client_stop(s_mqtt_client);
        s_mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT disconnected");
    }
}

bool mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

const char* mqtt_get_status(void)
{
    if (s_mqtt_connected) {
        return "connected";
    } else if (!s_mqtt_initialized) {
        return "not_initialized";
    } else {
        return "disconnected";
    }
}

void mqtt_publish_tag_data(const char* json_data)
{
    if (!s_mqtt_client || !s_mqtt_connected || !json_data) {
        return;
    }

    if (strlen(s_mqtt_config.publish_topic) == 0) {
        ESP_LOGW(TAG, "Publish topic not configured");
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_mqtt_config.publish_topic, 
                                        json_data, 0, 1, 0);
    ESP_LOGI(TAG, "Published tag data, msg_id=%d", msg_id);
}

void mqtt_publish_status(const char* status)
{
    if (!s_mqtt_client || !status) {
        return;
    }

    // Create status topic based on publish topic
    char status_topic[256];
    if (strlen(s_mqtt_config.publish_topic) > 0) {
        snprintf(status_topic, sizeof(status_topic), "%s/status", s_mqtt_config.publish_topic);
    } else {
        strcpy(status_topic, "rfid/status");
    }

    esp_mqtt_client_publish(s_mqtt_client, status_topic, status, 0, 1, 1); // retained
}