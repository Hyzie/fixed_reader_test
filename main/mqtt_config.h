#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <stdbool.h>

// Data buffer for offline storage
#define MQTT_BUFFER_SIZE 20
#define MQTT_DATA_MAX_LEN 512

typedef struct {
    char topic[128];
    char data[MQTT_DATA_MAX_LEN];
    uint32_t timestamp;
    bool occupied;
} mqtt_buffer_entry_t;

// MQTT Configuration
typedef struct {
    char broker_uri[128];     // mqtt://broker.example.com:1883
    char client_id[64];       // Unique client ID
    char username[64];        // MQTT username (optional)
    char password[64];        // MQTT password (optional)
    char publish_topic[128];  // Topic to publish tag data
    char subscribe_topic[128]; // Topic to subscribe for commands
} mqtt_config_t;

// MQTT Functions
void mqtt_init(void);
bool mqtt_is_connected(void);
bool mqtt_is_connecting(void);  // New function
const char* mqtt_get_status(void);

// Configuration
void mqtt_set_config(const mqtt_config_t* config);
void mqtt_get_config(mqtt_config_t* config);
int mqtt_save_config(const mqtt_config_t* config);
int mqtt_load_config(mqtt_config_t* config);

// Publishing with buffering
void mqtt_publish_tag_data(const char* json_data);
void mqtt_publish_status(const char* status);
void mqtt_publish_response(const char* response_json);
void mqtt_publish_rfid_data(const char* rfid_data);
void mqtt_publish_periodic_batch(void);  // Periodic batch publishing
void mqtt_publish_buffered(const char* topic, const char* data); // New buffered publish
void mqtt_flush_buffer(void); // Flush pending data when reconnected
void mqtt_save_buffer_to_nvs(void); // Save buffer to NVS for persistence
void mqtt_load_buffer_from_nvs(void); // Load buffer from NVS after restart
bool mqtt_health_check(void); // Check connection health
void mqtt_connection_monitor(void); // Monitor and maintain connection

// Command processing
void mqtt_process_command(const char* topic, int topic_len, const char* data, int data_len);

// Control
void mqtt_connect(void);
void mqtt_disconnect(void);

#endif // MQTT_CLIENT_H