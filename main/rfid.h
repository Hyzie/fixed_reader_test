#ifndef RFID_H
#define RFID_H

#include "uart.h"
#include <stdint.h>

void rfid_init(void);
void rfid_start_inventory(void);
void rfid_stop_inventory(void);
const char* rfid_get_status(void);
const char* rfid_get_last_command(void);
void rfid_set_last_command(const char* cmd_description);

// Power control functions
void rfid_set_power(int pwr1, int pwr2, int pwr3, int pwr4);
void rfid_get_power(int *pwr1, int *pwr2, int *pwr3, int *pwr4);

// Process raw bytes received from reader (call from UART rx task)
void rfid_process_bytes(const uint8_t *buf, size_t len);
// Fill provided buffer with JSON array of recent tags. Returns number of bytes written (not including terminating NUL)
int rfid_get_tags_json(char *out, int out_len);

#endif // RFID_H
