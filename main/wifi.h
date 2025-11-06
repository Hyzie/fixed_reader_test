#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>

// WiFi initialization and management
void wifi_init(void);
bool wifi_is_connected(void);
const char* wifi_get_status(void);
void wifi_connect_with_credentials(const char* ssid, const char* password);
void wifi_disconnect(void);

// WiFi connection testing
bool wifi_test_connection(const char* ssid, const char* password);

// Get current WiFi info
const char* wifi_get_connected_ssid(void);
const char* wifi_get_ip_address(void);

#endif // WIFI_H