/* uart.h - UART module public API */
#ifndef UART_H
#define UART_H

#include <stddef.h>

void uart_init(int UART_TXD, int UART_RXD);
void uart_start_rx_task(void);
int uart_get_rx_data(char *dest, int max_len); // copies data into dest and clears internal buffer, returns bytes copied
void uart_send_bytes(const char *data, size_t len);

#endif // UART_H
