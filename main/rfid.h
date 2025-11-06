#ifndef RFID_H
#define RFID_H

#include "uart.h"
#include <stdint.h>
#include <stdbool.h>

void rfid_init(void);
void rfid_start_inventory(void);
void rfid_stop_inventory(void);
void rfid_start_inventory_local(void);  // Local version (no MQTT)
void rfid_stop_inventory_local(void);   // Local version (no MQTT)  
void rfid_start_inventory_mqtt(void);   // MQTT version (with MQTT publishing)
void rfid_stop_inventory_mqtt(void);    // MQTT version (with MQTT publishing)
const char* rfid_get_status(void);
const char* rfid_get_last_command(void);
void rfid_set_last_command(const char* cmd_description);

// Power control functions
void rfid_set_power(int pwr1, int pwr2, int pwr3, int pwr4);
void rfid_get_power(int *pwr1, int *pwr2, int *pwr3, int *pwr4);
void rfid_query_power(void);  // Send power query command without returning values

// Reader information and connection functions (based on NRN SDK)
void rfid_query_reader_info(void);
void rfid_confirm_connection(void);

// Process raw bytes received from reader (call from UART rx task)
void rfid_process_bytes(const uint8_t *buf, size_t len);
// Fill provided buffer with JSON array of recent tags. Returns number of bytes written (not including terminating NUL)
int rfid_get_tags_json(char *out, int out_len);
// Reset startup delay for immediate tag processing (used when manually starting inventory)
void rfid_reset_startup_delay(void);

// Status functions
const char* rfid_get_local_status(void);   // Local/web server status
const char* rfid_get_mqtt_status(void);    // MQTT/remote status
bool rfid_get_mqtt_status_bool(void);      // MQTT status as boolean
int rfid_get_mqtt_tags_json(char *out, int out_len);  // MQTT tags JSON

// MQTT command handlers
void rfid_handle_inventory_command(const char* action);
void rfid_handle_power_command(const char* action, int ant1, int ant2, int ant3, int ant4);

#endif // RFID_H
