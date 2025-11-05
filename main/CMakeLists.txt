idf_component_register(SRCS "main.c" "uart.c" "eth.c" "web.c" "rfid.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_eth esp_event esp_netif driver esp_http_server nvs_flash
                    PRIV_REQUIRES esp_timer)
