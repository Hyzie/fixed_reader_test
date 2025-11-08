#include "sdkconfig.h"
#include "eth.h"
#include "network_config.h"
#include <stdio.h>
#include <string.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_netif_glue.h"

static const char *TAG = "ETH";

// Ethernet connection status
static bool s_eth_connected = false;

// --- Cấu hình chân cho W5500 ---
#define PIN_SCLK  36
#define PIN_MOSI  37
#define PIN_MISO  35
#define PIN_CS    9
#define PIN_INT   14
#define PIN_RST   7

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        s_eth_connected = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP: " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW: " IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    // Mark Ethernet as connected
    s_eth_connected = true;

    // Spawn a connectivity test task to verify Internet access (non-blocking)
    // xTaskCreate(connectivity_test_task, "eth_connect_test", 4096, NULL, 5, NULL);
}

void eth_init(void)
{
    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create new Ethernet network interface with DHCP disabled
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    
    // Disable DHCP client (we'll use static IP)
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
    
    // Configure static IP settings
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, STATIC_IP_OCTET1, STATIC_IP_OCTET2, STATIC_IP_OCTET3, STATIC_IP_OCTET4);        // Static IP from config
    IP4_ADDR(&ip_info.gw, GATEWAY_OCTET1, GATEWAY_OCTET2, GATEWAY_OCTET3, GATEWAY_OCTET4);          // Gateway from config
    IP4_ADDR(&ip_info.netmask, NETMASK_OCTET1, NETMASK_OCTET2, NETMASK_OCTET3, NETMASK_OCTET4);     // Subnet mask from config
    
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
    
    ESP_LOGI(TAG, "Static IP configured: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
    
    // Configure DNS servers (optional but recommended for internet access)
    esp_netif_dns_info_t dns_info;
    IP4_ADDR(&dns_info.ip.u_addr.ip4, PRIMARY_DNS_OCTET1, PRIMARY_DNS_OCTET2, PRIMARY_DNS_OCTET3, PRIMARY_DNS_OCTET4);              // Primary DNS from config
    ESP_ERROR_CHECK(esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info));
    
    IP4_ADDR(&dns_info.ip.u_addr.ip4, SECONDARY_DNS_OCTET1, SECONDARY_DNS_OCTET2, SECONDARY_DNS_OCTET3, SECONDARY_DNS_OCTET4);            // Secondary DNS from config
    ESP_ERROR_CHECK(esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info));
    
    ESP_LOGI(TAG, "DNS servers configured from network_config.h");

    // Init GPIO ISR service
    gpio_install_isr_service(0);

    // Init SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Init W5500
    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = PIN_CS,
        .queue_size = 20
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500_config.int_gpio_num = PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = PIN_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    // Set MAC address
    uint8_t mac_addr[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Start Ethernet
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

bool eth_is_connected(void)
{
    return s_eth_connected;
}
