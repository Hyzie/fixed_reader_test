#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stddef.h>

int wifi_config_save(const char *ssid, const char *pass);
int wifi_config_load(char *ssid_buf, size_t ssid_len, char *pass_buf, size_t pass_len);

#endif // WIFI_CONFIG_H
