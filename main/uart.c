#include "uart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "rfid.h"

static const char *TAG = "UART";

// UART pins and settings (kept from original)
#define UART_PORT UART_NUM_2
#define BUF_SIZE (1024)

static QueueHandle_t uart_queue;
static char rx_buffer[BUF_SIZE];
static int rx_buffer_len = 0;

void uart_init(int UART_TXD, int UART_RXD)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TXD, UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART initialized");
}

void uart_send_bytes(const char *data, size_t len)
{
    if (data == NULL || len == 0) return;
    
    // Capture command for status display
    char cmd_str[512] = "TX: ";
    size_t offset = 4; // Start after "TX: "
    
    for (size_t i = 0; i < len && offset < sizeof(cmd_str) - 4; i++) {
        snprintf(cmd_str + offset, sizeof(cmd_str) - offset, "%02X ", (uint8_t)data[i]);
        offset += 3; // Each byte takes 3 characters ("XX ")
    }
    
    // Remove trailing space and add null terminator
    if (offset > 4) {
        cmd_str[offset - 1] = '\0';
    }
    
    // Update the last command in RFID status
    rfid_set_last_command(cmd_str);
    
    // Send the actual command
    int bytes_written = uart_write_bytes(UART_PORT, data, len);
    ESP_LOGI(TAG, "UART sent %d bytes: %s", bytes_written, cmd_str);
}

int uart_get_rx_data(char *dest, int max_len)
{
    if (dest == NULL || max_len <= 0) return 0;
    int to_copy = rx_buffer_len;
    if (to_copy > max_len - 1) to_copy = max_len - 1;
    if (to_copy > 0) {
        memcpy(dest, rx_buffer, to_copy);
        dest[to_copy] = '\0';
        // clear internal buffer
        rx_buffer_len = 0;
    } else {
        if (max_len > 0) dest[0] = '\0';
    }
    return to_copy;
}

static void uart_rx_task(void *arg)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE);

    while (1) {
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA: {
                    int len = uart_read_bytes(UART_PORT, dtmp, event.size, portMAX_DELAY);
                    if (len > 0) {
                        // Process raw bytes for RFID parsing first
                        rfid_process_bytes(dtmp, len);
                        
                        // Store for web terminal (with buffer overflow protection)
                        if (rx_buffer_len + len < BUF_SIZE - 1) {
                            memcpy(rx_buffer + rx_buffer_len, dtmp, len);
                            rx_buffer_len += len;
                            rx_buffer[rx_buffer_len] = '\0';
                        } else {
                            // Buffer full, clear and start fresh
                            rx_buffer_len = 0;
                            if (len < BUF_SIZE - 1) {
                                memcpy(rx_buffer, dtmp, len);
                                rx_buffer_len = len;
                                rx_buffer[rx_buffer_len] = '\0';
                            }
                        }
                        ESP_LOGD(TAG, "UART RX: %d bytes processed", len);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_queue);
                    break;
                default:
                    break;
            }
        }
    }
    free(dtmp);
}

void uart_start_rx_task(void)
{
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 10, NULL);
}
