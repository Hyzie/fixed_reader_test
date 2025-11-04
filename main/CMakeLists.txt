idf_component_register(SRCS "main.c" "uart.c" "eth.c" "web.c" "rfid.c" "wifi_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_eth esp_event esp_netif driver esp_http_server esp_http_client nvs_flash
                    PRIV_REQUIRES esp_timer)
