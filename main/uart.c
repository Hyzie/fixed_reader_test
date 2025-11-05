#include "uart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "rfid.h"

static const char *TAG = "UART";

// UART pins and settings (kept from original)
#define UART_PORT UART_NUM_1
#define BUF_SIZE (4096)  // Increased buffer size for high-speed tag data

static QueueHandle_t uart_queue;
static char rx_buffer[BUF_SIZE];
static int rx_buffer_len = 0;
static bool uart_initialized = false;

void uart_init(int UART_TXD, int UART_RXD)
{
    // Try common RFID reader settings first
    uart_config_t uart_config = {
        .baud_rate = 115200,         
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, BUF_SIZE * 4, BUF_SIZE * 4, 30, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TXD, UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uart_initialized = true;
    ESP_LOGI(TAG, "UART initialized on TXD=%d, RXD=%d, baud=%d", UART_TXD, UART_RXD, uart_config.baud_rate);
}

void uart_send_bytes(const char *data, size_t len)
{
    if (data == NULL || len == 0) return;
    
    // Check if UART driver is initialized
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized, cannot send data");
        return;
    }
    
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
    if (bytes_written < 0) {
        ESP_LOGE(TAG, "UART write failed with error %d: %s", bytes_written, cmd_str);
    } else {
        ESP_LOGI(TAG, "UART sent %d bytes: %s", bytes_written, cmd_str);
    }
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
    ESP_LOGI(TAG, "UART RX task started and waiting for data...");
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE);

    while (1) {
        // Yield control more frequently to prevent watchdog timeout
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(100))) {
            // Reduce event logging to prevent console blocking during data floods
            static int event_log_count = 0;
            if (++event_log_count % 500 == 0) {
                ESP_LOGI(TAG, "UART event received, type: %d (count: %d)", event.type, event_log_count);
            }
            
            switch (event.type) {
                case UART_DATA: {
                    // Yield immediately to prevent watchdog timeout during high-speed processing
                    static int immediate_yield_count = 0;
                    if (++immediate_yield_count % 3 == 0) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                    
                    int len = uart_read_bytes(UART_PORT, dtmp, event.size, portMAX_DELAY);
                    if (len > 0) {
                        // Extremely minimal logging to prevent watchdog timeout
                        static int packet_count = 0;
                        packet_count++;
                        
                        // Only print every 1000th packet during high-speed operation
                        if (packet_count % 1000 == 0) {
                            printf("RX: %dk packets\n", packet_count / 1000);
                            // Longer yield after console output
                            vTaskDelay(pdMS_TO_TICKS(5));
                        }
                        
                        // Process the data
                        rfid_process_bytes(dtmp, len);
                        
                        // Simplified hex storage to reduce processing time during data floods
                        // Only store if there's enough space, otherwise skip to prevent blocking
                        int hex_space_needed = len * 3 + 10; // Simplified calculation
                        if (rx_buffer_len + hex_space_needed < BUF_SIZE - 100) {
                            // Simple hex append without line breaks during high-speed operation
                            char *write_pos = rx_buffer + rx_buffer_len;
                            for (int i = 0; i < len && i < 32; i++) { // Limit to 32 bytes per packet
                                write_pos += sprintf(write_pos, "%02X ", dtmp[i]);
                            }
                            if (len > 32) {
                                write_pos += sprintf(write_pos, "... ");
                            }
                            write_pos += sprintf(write_pos, "\n");
                            rx_buffer_len = write_pos - rx_buffer;
                        } else {
                            // Buffer getting full, clear old data
                            rx_buffer_len = 0;
                        }
                        
                        // Yield every 5 packets to prevent watchdog timeout
                        if (packet_count % 5 == 0) {
                            vTaskDelay(pdMS_TO_TICKS(1));
                        }
                        
                        ESP_LOGD(TAG, "UART RX: %d bytes processed", len);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow - clearing buffer");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_queue);
                    // Clear our internal buffer too
                    rx_buffer_len = 0;
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full - clearing buffer");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_queue);
                    // Clear our internal buffer too
                    rx_buffer_len = 0;
                    break;
                default:
                    break;
            }
        } else {
            // Timeout occurred - yield control to prevent watchdog timeout
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    free(dtmp);
}

void uart_start_rx_task(void)
{
    printf("Starting UART RX task...\n");
    // Lower priority and larger stack for high-speed tag processing
    // Priority 5 instead of 10 to prevent blocking other tasks
    xTaskCreate(uart_rx_task, "uart_rx_task", 8192, NULL, 5, NULL);
    printf("UART RX task created\n");
}
