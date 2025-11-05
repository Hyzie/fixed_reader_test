#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <stdbool.h>

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
const char* mqtt_get_status(void);

// Configuration
void mqtt_set_config(const mqtt_config_t* config);
void mqtt_get_config(mqtt_config_t* config);
int mqtt_save_config(const mqtt_config_t* config);
int mqtt_load_config(mqtt_config_t* config);

// Publishing
void mqtt_publish_tag_data(const char* json_data);
void mqtt_publish_status(const char* status);

// Control
void mqtt_connect(void);
void mqtt_disconnect(void);

#endif // MQTT_CLIENT_H