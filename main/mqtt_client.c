#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
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
#include "rfid.h"
#include "cJSON.h"

// Global variables for throttling
static uint64_t s_last_publish_time = 0;
static const uint64_t PUBLISH_INTERVAL_MS = 500;  // Publish every 500ms to balance real-time vs stability

static const char *TAG = "MQTT";
static const char *NVS_NAMESPACE = "mqtt_cfg";

// Data buffering system for offline storage
static mqtt_buffer_entry_t s_mqtt_buffer[MQTT_BUFFER_SIZE];
static int s_buffer_head = 0;
static int s_buffer_count = 0;
static bool s_buffer_initialized = false;

// Connection health monitoring
static uint32_t s_last_successful_publish = 0;
static uint32_t s_connection_health_failures = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static mqtt_config_t s_mqtt_config = {0};
static bool s_mqtt_connected = false;
static bool s_mqtt_connecting = false;  // Add connecting state
static uint64_t s_connection_start_time = 0;  // Track connection start time
static bool s_mqtt_initialized = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        s_mqtt_connected = true;
        s_mqtt_connecting = false;  // Clear connecting state
        
        // Subscribe to command topics
        char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "reader/%s/cmd/rfid", s_mqtt_config.client_id);
        esp_mqtt_client_subscribe(client, cmd_topic, 1);
        ESP_LOGI(TAG, "Subscribed to RFID commands: %s", cmd_topic);
        
        snprintf(cmd_topic, sizeof(cmd_topic), "reader/%s/cmd/power", s_mqtt_config.client_id);
        esp_mqtt_client_subscribe(client, cmd_topic, 1);
        ESP_LOGI(TAG, "Subscribed to power commands: %s", cmd_topic);
        
        snprintf(cmd_topic, sizeof(cmd_topic), "reader/%s/cmd/inventory", s_mqtt_config.client_id);
        esp_mqtt_client_subscribe(client, cmd_topic, 1);
        ESP_LOGI(TAG, "Subscribed to inventory commands: %s", cmd_topic);
        
        // Subscribe to legacy command topic if configured
        if (strlen(s_mqtt_config.subscribe_topic) > 0) {
            int msg_id = esp_mqtt_client_subscribe(client, s_mqtt_config.subscribe_topic, 1);
            ESP_LOGI(TAG, "Subscribed to legacy topic %s, msg_id=%d", s_mqtt_config.subscribe_topic, msg_id);
        }
        
        // Publish connection status
        mqtt_publish_status("online");
        
        // Flush any buffered data after successful connection
        mqtt_flush_buffer();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT Disconnected");
        s_mqtt_connected = false;
        s_mqtt_connecting = false;  // Clear connecting state on disconnect
        
        // Schedule automatic reconnection after a short delay
        // The main task will handle the reconnection
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
        
        // Process received commands
        mqtt_process_command(event->topic, event->topic_len, event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Error");
        s_mqtt_connected = false;  // Mark as disconnected on error
        s_mqtt_connecting = false; // Clear connecting state on error
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
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

    // Initialize data buffer
    if (!s_buffer_initialized) {
        memset(s_mqtt_buffer, 0, sizeof(s_mqtt_buffer));
        s_buffer_head = 0;
        s_buffer_count = 0;
        s_buffer_initialized = true;
        ESP_LOGI(TAG, "MQTT data buffer initialized (%d entries)", MQTT_BUFFER_SIZE);
        
        // Load any persisted data from NVS
        mqtt_load_buffer_from_nvs();
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

    // Prevent multiple client instances
    if (s_mqtt_client) {
        if (s_mqtt_connected) {
            ESP_LOGI(TAG, "MQTT already connected");
            return;
        }
        if (s_mqtt_connecting) {
            ESP_LOGI(TAG, "MQTT connection already in progress");
            return;
        }
        ESP_LOGI(TAG, "Stopping existing MQTT client");
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for cleanup
    }

    s_mqtt_connecting = true;  // Set connecting state
    s_connection_start_time = esp_timer_get_time() / 1000ULL; // Store start time in ms

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_config.broker_uri,
        .broker.verification.certificate = NULL,  // Use default CA certificates
        .broker.verification.skip_cert_common_name_check = false,
        .broker.verification.use_global_ca_store = false,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        
        // Optimized for maximum stability and zero data loss
        .session.keepalive = 60,                    // Optimal keepalive for stability
        .session.disable_clean_session = false,    // Clean session for reliability  
        .session.disable_keepalive = false,        // Enable keepalive
        .network.timeout_ms = 30000,               // Longer timeout for stability
        .network.refresh_connection_after_ms = 0,  // Disable auto refresh
        .network.reconnect_timeout_ms = 5000,      // Fast reconnection
        .buffer.size = 16384,                      // Large input buffer 
        .buffer.out_size = 32768,                  // Very large output buffer for queuing
        .task.priority = 5,                        // Higher task priority
        .task.stack_size = 8192,                   // Larger stack for stability
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

bool mqtt_is_connecting(void)
{
    if (!s_mqtt_connecting) {
        return false;
    }
    
    // Check for connection timeout (30 seconds)
    uint64_t current_time = esp_timer_get_time() / 1000ULL;
    if (current_time - s_connection_start_time > 30000) {
        ESP_LOGW(TAG, "MQTT connection timeout, resetting connecting state");
        s_mqtt_connecting = false;
        return false;
    }
    
    return s_mqtt_connecting;
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

// Process incoming MQTT commands
void mqtt_process_command(const char* topic, int topic_len, const char* data, int data_len)
{
    if (!topic || !data || topic_len <= 0 || data_len <= 0) {
        return;
    }
    
    // Copy topic and data to null-terminated strings
    char topic_str[128];
    char data_str[256];
    
    int copy_len = (topic_len < sizeof(topic_str) - 1) ? topic_len : sizeof(topic_str) - 1;
    strncpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';
    
    copy_len = (data_len < sizeof(data_str) - 1) ? data_len : sizeof(data_str) - 1;
    strncpy(data_str, data, copy_len);
    data_str[copy_len] = '\0';
    
    ESP_LOGI(TAG, "Processing command - Topic: %s, Data: %s", topic_str, data_str);
    
    // Parse JSON command
    cJSON *json = cJSON_Parse(data_str);
    if (!json) {
        mqtt_publish_response("{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
        return;
    }
    
    // Check if it's an RFID command
    if (strstr(topic_str, "/cmd/rfid") != NULL) {
        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action)) {
            ESP_LOGI(TAG, "Executing RFID command: %s", action->valuestring);
            rfid_handle_inventory_command(action->valuestring);
        } else {
            mqtt_publish_response("{\"command\":\"rfid\",\"status\":\"error\",\"message\":\"Missing action parameter\"}");
        }
    }
    // Check if it's a power command
    else if (strstr(topic_str, "/cmd/power") != NULL) {
        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action)) {
            ESP_LOGI(TAG, "Executing power command: %s", action->valuestring);
            
            if (strcmp(action->valuestring, "set") == 0) {
                // Get power values from JSON
                cJSON *ant1 = cJSON_GetObjectItem(json, "ant1");
                cJSON *ant2 = cJSON_GetObjectItem(json, "ant2");
                cJSON *ant3 = cJSON_GetObjectItem(json, "ant3");
                cJSON *ant4 = cJSON_GetObjectItem(json, "ant4");
                
                int p1 = (ant1 && cJSON_IsNumber(ant1)) ? ant1->valueint : 30;
                int p2 = (ant2 && cJSON_IsNumber(ant2)) ? ant2->valueint : 30;
                int p3 = (ant3 && cJSON_IsNumber(ant3)) ? ant3->valueint : 30;
                int p4 = (ant4 && cJSON_IsNumber(ant4)) ? ant4->valueint : 30;
                
                rfid_handle_power_command("set", p1, p2, p3, p4);
            } else if (strcmp(action->valuestring, "status") == 0) {
                // Return simple power status
                mqtt_publish_response("{\"command\":\"power\",\"action\":\"status\",\"status\":\"success\",\"power_state\":\"on\",\"message\":\"RFID module is powered on\"}");
            } else if (strcmp(action->valuestring, "get") == 0) {
                // Get detailed antenna power levels
                rfid_handle_power_command("query", 0, 0, 0, 0);
            } else {
                rfid_handle_power_command(action->valuestring, 0, 0, 0, 0);
            }
        } else {
            mqtt_publish_response("{\"command\":\"power\",\"status\":\"error\",\"message\":\"Missing action parameter\"}");
        }
    }
    // Check if it's an inventory command  
    else if (strstr(topic_str, "/cmd/inventory") != NULL) {
        // For backward compatibility, handle both JSON and simple string commands
        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action)) {
            ESP_LOGI(TAG, "Executing inventory command: %s", action->valuestring);
            rfid_handle_inventory_command(action->valuestring);
        } else {
            // Fallback to treating the entire data as action for simple commands
            ESP_LOGI(TAG, "Executing simple inventory command: %s", data_str);
            rfid_handle_inventory_command(data_str);
        }
    }
    else {
        ESP_LOGW(TAG, "Unknown command topic: %s", topic_str);
        mqtt_publish_response("{\"status\":\"error\",\"message\":\"Unknown command topic\"}");
    }
    
    cJSON_Delete(json);
}

// Publish command responses
void mqtt_publish_response(const char* response_json)
{
    if (!s_mqtt_client || !response_json) {
        return;
    }
    
    char response_topic[256];
    snprintf(response_topic, sizeof(response_topic), "reader/%s/data/response", s_mqtt_config.client_id);
    
    esp_mqtt_client_publish(s_mqtt_client, response_topic, response_json, 0, 0, 0);  // QoS 0 for speed
    ESP_LOGI(TAG, "Published response: %s", response_json);
}

// Publish RFID data to MQTT (called from UART when data is received)
void mqtt_publish_rfid_data(const char* rfid_data)
{
    if (!s_mqtt_client || !rfid_data) {
        return;
    }
    
    // Throttle publishing to prevent MQTT overload
    uint64_t current_time = esp_timer_get_time() / 1000ULL;  // Convert to milliseconds
    if (current_time - s_last_publish_time < PUBLISH_INTERVAL_MS) {
        // Skip this publish to avoid overloading MQTT
        static int throttle_count = 0;
        if (++throttle_count % 500 == 0) {  // Reduced frequency of throttle messages
            printf("MQTT: Throttling data (skipped %d publishes)\n", throttle_count);
        }
        return;  
    }
    s_last_publish_time = current_time;
    
    char data_topic[256];
    snprintf(data_topic, sizeof(data_topic), "reader/%s/data/tags", s_mqtt_config.client_id);
    
    char data_json[1024];
    snprintf(data_json, sizeof(data_json), 
             "{\"raw_data\":\"%s\",\"timestamp\":%lu,\"device_id\":\"%s\"}", 
             rfid_data, (unsigned long)time(NULL), s_mqtt_config.client_id);
    
    esp_mqtt_client_publish(s_mqtt_client, data_topic, data_json, 0, 0, 0);  // QoS 0 for speed
    // Reduced logging for performance during high-frequency tag detection
    static int log_count = 0;
    if (++log_count % 10 == 0) {  // Log every 10th publish
        ESP_LOGI(TAG, "Published RFID data (count: %d)", log_count);
    }
}

// Periodic MQTT publishing function - publishes batch of recent tags
void mqtt_publish_periodic_batch(void)
{
    if (!s_mqtt_client) {
        return;
    }
    
    // Only publish when MQTT inventory is running
    extern bool rfid_get_mqtt_status_bool(void);
    if (!rfid_get_mqtt_status_bool()) {
        return;
    }
    
    char data_topic[256];
    snprintf(data_topic, sizeof(data_topic), "reader/%s/data/batch", s_mqtt_config.client_id);
    
    // Use static buffer to reduce stack usage and track data changes
    static char batch_json[1536]; // Reduced size, static allocation
    static uint32_t last_tag_count = 0;
    extern int rfid_get_mqtt_tags_json(char *out, int out_len);
    int used = rfid_get_mqtt_tags_json(batch_json, sizeof(batch_json));
    
    if (used > 0) {
        // Extract active tag count from JSON for change detection
        // Simple parsing to get active_tags count
        char *count_start = strstr(batch_json, "\"active_tags\":");
        uint32_t current_tag_count = 0;
        if (count_start) {
            sscanf(count_start + 14, "%lu", (unsigned long*)&current_tag_count);
        }
        
        // Only publish if tag count changed or significant data size
        if (current_tag_count != last_tag_count || used > 200) {
            mqtt_publish_buffered(data_topic, batch_json);
            printf("MQTT: Queued tag batch (%d bytes, %lu tags)\n", used, (unsigned long)current_tag_count);
            last_tag_count = current_tag_count;
        } else {
            printf("MQTT: Skipping batch publish (tags: %lu, no changes)\n", (unsigned long)current_tag_count);
        }
    } else {
        printf("MQTT: No MQTT tags to publish\n");
    }
}

// Buffer management functions for zero data loss
static void mqtt_buffer_add(const char* topic, const char* data)
{
    if (!topic || !data || !s_buffer_initialized) return;
    
    // Find next available slot (circular buffer)
    int index = s_buffer_head;
    
    // If buffer is full, overwrite oldest entry
    if (s_buffer_count >= MQTT_BUFFER_SIZE) {
        ESP_LOGW(TAG, "Buffer full, overwriting oldest entry");
    } else {
        s_buffer_count++;
    }
    
    // Store data
    strncpy(s_mqtt_buffer[index].topic, topic, sizeof(s_mqtt_buffer[index].topic) - 1);
    strncpy(s_mqtt_buffer[index].data, data, sizeof(s_mqtt_buffer[index].data) - 1);
    s_mqtt_buffer[index].timestamp = esp_timer_get_time() / 1000ULL; // milliseconds
    s_mqtt_buffer[index].occupied = true;
    
    // Move head pointer
    s_buffer_head = (s_buffer_head + 1) % MQTT_BUFFER_SIZE;
    
    ESP_LOGI(TAG, "Buffered data: %s (%d bytes, buffer: %d/%d)", 
             topic, (int)strlen(data), s_buffer_count, MQTT_BUFFER_SIZE);
}

void mqtt_publish_buffered(const char* topic, const char* data)
{
    if (!topic || !data) return;
    
    // Try to publish immediately if connected
    if (s_mqtt_connected && s_mqtt_client) {
        esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, 0, 0);
        s_last_successful_publish = esp_timer_get_time() / 1000ULL; // Update health tracking
        ESP_LOGI(TAG, "Published immediately: %s", topic);
    } else {
        // Store in buffer for later
        mqtt_buffer_add(topic, data);
    }
}

void mqtt_flush_buffer(void)
{
    if (!s_buffer_initialized || s_buffer_count == 0) return;
    
    ESP_LOGI(TAG, "Flushing %d buffered messages", s_buffer_count);
    
    int flushed = 0;
    int start_index = (s_buffer_head - s_buffer_count + MQTT_BUFFER_SIZE) % MQTT_BUFFER_SIZE;
    
    for (int i = 0; i < s_buffer_count && flushed < 10; i++) { // Limit to 10 per flush to avoid overload
        int index = (start_index + i) % MQTT_BUFFER_SIZE;
        
        if (s_mqtt_buffer[index].occupied) {
            // Attempt to publish
            esp_mqtt_client_publish(s_mqtt_client, 
                                   s_mqtt_buffer[index].topic, 
                                   s_mqtt_buffer[index].data, 
                                   0, 0, 0);
            
            // Mark as sent
            s_mqtt_buffer[index].occupied = false;
            flushed++;
            
            ESP_LOGI(TAG, "Flushed: %s", s_mqtt_buffer[index].topic);
        }
    }
    
    // Update buffer count
    s_buffer_count -= flushed;
    if (s_buffer_count < 0) s_buffer_count = 0;
    
    ESP_LOGI(TAG, "Flushed %d messages, %d remaining in buffer", flushed, s_buffer_count);
}

// NVS persistence for critical data
void mqtt_save_buffer_to_nvs(void)
{
    if (!s_buffer_initialized || s_buffer_count == 0) return;
    
    nvs_handle_t h;
    esp_err_t err = nvs_open("mqtt_buf", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for buffer save: %s", esp_err_to_name(err));
        return;
    }
    
    // Save buffer count and data
    nvs_set_i32(h, "buf_count", s_buffer_count);
    
    int saved = 0;
    int start_index = (s_buffer_head - s_buffer_count + MQTT_BUFFER_SIZE) % MQTT_BUFFER_SIZE;
    
    for (int i = 0; i < s_buffer_count && saved < 10; i++) { // Limit NVS saves
        int index = (start_index + i) % MQTT_BUFFER_SIZE;
        
        if (s_mqtt_buffer[index].occupied) {
            char key_topic[32], key_data[32], key_ts[32];  // Increased buffer sizes
            snprintf(key_topic, sizeof(key_topic), "topic_%d", saved);
            snprintf(key_data, sizeof(key_data), "data_%d", saved);
            snprintf(key_ts, sizeof(key_ts), "ts_%d", saved);
            
            nvs_set_str(h, key_topic, s_mqtt_buffer[index].topic);
            nvs_set_str(h, key_data, s_mqtt_buffer[index].data);
            nvs_set_u32(h, key_ts, s_mqtt_buffer[index].timestamp);
            saved++;
        }
    }
    
    nvs_set_i32(h, "buf_saved", saved);
    nvs_commit(h);
    nvs_close(h);
    
    ESP_LOGI(TAG, "Saved %d critical messages to NVS", saved);
}

void mqtt_load_buffer_from_nvs(void)
{
    if (!s_buffer_initialized) return;
    
    nvs_handle_t h;
    esp_err_t err = nvs_open("mqtt_buf", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No previous buffer data in NVS");
        return;
    }
    
    int32_t saved_count = 0;
    size_t required_size;
    
    err = nvs_get_i32(h, "buf_saved", &saved_count);
    if (err != ESP_OK || saved_count <= 0) {
        nvs_close(h);
        return;
    }
    
    ESP_LOGI(TAG, "Loading %d messages from NVS", (int)saved_count);
    
    for (int i = 0; i < saved_count && i < MQTT_BUFFER_SIZE; i++) {
        char key_topic[32], key_data[32], key_ts[32];  // Increased buffer sizes
        snprintf(key_topic, sizeof(key_topic), "topic_%d", i);
        snprintf(key_data, sizeof(key_data), "data_%d", i);
        snprintf(key_ts, sizeof(key_ts), "ts_%d", i);
        
        required_size = sizeof(s_mqtt_buffer[i].topic);
        err = nvs_get_str(h, key_topic, s_mqtt_buffer[i].topic, &required_size);
        if (err != ESP_OK) continue;
        
        required_size = sizeof(s_mqtt_buffer[i].data);
        err = nvs_get_str(h, key_data, s_mqtt_buffer[i].data, &required_size);
        if (err != ESP_OK) continue;
        
        uint32_t timestamp;
        err = nvs_get_u32(h, key_ts, &timestamp);
        if (err == ESP_OK) {
            s_mqtt_buffer[i].timestamp = timestamp;
            s_mqtt_buffer[i].occupied = true;
            s_buffer_count++;
        }
    }
    
    s_buffer_head = s_buffer_count % MQTT_BUFFER_SIZE;
    nvs_close(h);
    
    ESP_LOGI(TAG, "Loaded %d messages from NVS to buffer", s_buffer_count);
}

// Connection health monitoring constants
static const uint32_t HEALTH_CHECK_INTERVAL = 30000; // 30 seconds

bool mqtt_health_check(void)
{
    if (!s_mqtt_connected) return false;
    
    uint32_t now = esp_timer_get_time() / 1000ULL;
    
    // Check if we haven't published successfully in a while
    if (s_last_successful_publish > 0 && (now - s_last_successful_publish) > HEALTH_CHECK_INTERVAL) {
        s_connection_health_failures++;
        ESP_LOGW(TAG, "Health check failed: No successful publish for %lu ms (failures: %lu)", 
                (unsigned long)(now - s_last_successful_publish), (unsigned long)s_connection_health_failures);
        
        // Force reconnection after multiple failures
        if (s_connection_health_failures >= 3) {
            ESP_LOGW(TAG, "Forcing reconnection due to health failures");
            mqtt_disconnect();
            return false;
        }
    }
    
    return true;
}

void mqtt_connection_monitor(void)
{
    static uint32_t last_monitor_time = 0;
    uint32_t now = esp_timer_get_time() / 1000ULL;
    
    // Run monitoring every 30 seconds
    if ((now - last_monitor_time) < HEALTH_CHECK_INTERVAL) return;
    last_monitor_time = now;
    
    if (s_mqtt_connected) {
        // Update last successful time if connected
        s_last_successful_publish = now;
        s_connection_health_failures = 0;
        
        // Perform health check
        mqtt_health_check();
        
        // Save critical data to NVS periodically
        if (s_buffer_count > 5) {
            mqtt_save_buffer_to_nvs();
        }
    } else {
        ESP_LOGW(TAG, "Connection monitor: MQTT disconnected");
    }
    
    ESP_LOGI(TAG, "Connection health: %s, buffer: %d/%d, failures: %lu", 
             s_mqtt_connected ? "OK" : "DISCONNECTED", 
             s_buffer_count, MQTT_BUFFER_SIZE, 
             (unsigned long)s_connection_health_failures);
}