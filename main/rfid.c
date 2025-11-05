#include "rfid.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_timer.h"
#include "uart.h"

#define READER_TXD  17
#define READER_RXD  18

static const char *TAG = "RFID";
static volatile int s_running = 0;
static char s_last_command[256] = "No command sent yet";

// Store actual power values received from reader
static int s_power_values[4] = {30, 30, 30, 30}; // Default values
static volatile int s_power_request_pending = 0;

// Tag cleanup configuration
#define TAG_TIMEOUT_MS (30000)  // 30 seconds timeout for inactive tags
#define MAX_TAGS 64

// Tag storage
typedef struct {
    char epc[128];
    int rssi;
    int ant;
    uint64_t last_ms;
} tag_item_t;

static tag_item_t s_tags[MAX_TAGS];

// Clean up old tags periodically
static void cleanup_old_tags(void) {
    uint64_t now = esp_timer_get_time() / 1000ULL;
    int cleaned = 0;
    
    for (int i = 0; i < MAX_TAGS; ++i) {
        if (s_tags[i].epc[0] != '\0' && (now - s_tags[i].last_ms) > TAG_TIMEOUT_MS) {
            s_tags[i].epc[0] = '\0'; // Mark as empty
            cleaned++;
        }
    }
    
    if (cleaned > 0) {
        ESP_LOGI(TAG, "Cleaned up %d old tags", cleaned);
    }
}

static int find_tag_index(const char* epc) {
    for (int i = 0; i < MAX_TAGS; ++i) {
        if (s_tags[i].epc[0] != '\0' && strcmp(s_tags[i].epc, epc) == 0) return i;
    }
    return -1;
}

static int alloc_tag_index(const char* epc) {
    // First try to find an empty slot
    for (int i = 0; i < MAX_TAGS; ++i) {
        if (s_tags[i].epc[0] == '\0') { 
            strncpy(s_tags[i].epc, epc, sizeof(s_tags[i].epc)-1); 
            return i; 
        }
    }
    
    // If no empty slots, clean up old tags and try again
    cleanup_old_tags();
    for (int i = 0; i < MAX_TAGS; ++i) {
        if (s_tags[i].epc[0] == '\0') { 
            strncpy(s_tags[i].epc, epc, sizeof(s_tags[i].epc)-1); 
            return i; 
        }
    }
    
    // Still no space, overwrite oldest
    int oldest = 0; 
    uint64_t tmin = s_tags[0].last_ms;
    for (int i = 1; i < MAX_TAGS; ++i) {
        if (s_tags[i].last_ms < tmin) { 
            tmin = s_tags[i].last_ms; 
            oldest = i; 
        }
    }
    strncpy(s_tags[oldest].epc, epc, sizeof(s_tags[oldest].epc)-1);
    return oldest;
}

// helper: convert byte to hex chars
static inline void byte_to_hex(uint8_t b, char *out) { const char *h = "0123456789ABCDEF"; out[0]=h[b>>4]; out[1]=h[b&0xF]; }

static inline int ok_rssi(int v) { int a = v<0 ? -v : v; return (a>=20 && a<=100); }

// Parser: look for pattern E2 80 and preceding length byte, similar heuristic from Arduino code
static int extract_one_tag(const uint8_t* buf, size_t len, size_t startPos, size_t *nextPos, char *epc_out, size_t epc_out_len, int *rssi_out, int *ant_out) {
    for (size_t i = startPos; i + 4 < len; ++i) {
        if (buf[i] == 0xE2 && buf[i+1] == 0x80) {
            if (i == 0) continue;
            uint8_t L = buf[i-1];
            if (L < 4 || L > 32) continue;
            if (i + L > len) break;

            // epc is L bytes starting at i
            size_t written = 0;
            for (size_t k = 0; k < L && written + 2 < epc_out_len; ++k) {
                char h[2]; byte_to_hex(buf[i+k], h);
                epc_out[written++] = h[0]; epc_out[written++] = h[1];
            }
            epc_out[written] = '\0';

            // RSSI heuristics
            int r0 = 0, r1 = 0;
            if (i + L < len) r0 = - (int)buf[i+L];
            if (i + L + 1 < len) r1 = - (int)buf[i+L+1];
            *rssi_out = ok_rssi(r0) ? r0 : (ok_rssi(r1) ? r1 : 0);

            // ANT heuristics
            *ant_out = 0;
            uint8_t a2 = (i + L + 2 < len) ? buf[i+L+2] : 0;
            uint8_t a3 = (i + L + 3 < len) ? buf[i+L+3] : 0;
            if (a2 >=1 && a2 <=8) *ant_out = a2; else if (a3 >=1 && a3 <=8) *ant_out = a3;

            *nextPos = i + L + 4; if (*nextPos > len) *nextPos = len;
            return 1;
        }
    }
    return 0;
}

// Parse power response from reader
static void parse_power_response(const uint8_t *buf, size_t len) {
    // Expected response format: 5A 00 01 02 02 00 08 01 PWR1 02 PWR2 03 PWR3 04 PWR4 CRC CRC
    // Length should be 17 bytes for power response
    if (len >= 17 && buf[0] == 0x5A && buf[1] == 0x00 && buf[2] == 0x01 && buf[3] == 0x02 && buf[4] == 0x02) {
        // Check data length field (should be 0x00 0x08 = 8 bytes of antenna data)
        if (buf[5] == 0x00 && buf[6] == 0x08) {
            // Parse antenna power values
            // Format: ID PWR ID PWR ID PWR ID PWR
            // buf[7]=0x01, buf[8]=power1, buf[9]=0x02, buf[10]=power2, etc.
            
            if (buf[7] == 0x01) s_power_values[0] = buf[8];   // Antenna 1 power
            if (buf[9] == 0x02) s_power_values[1] = buf[10];  // Antenna 2 power  
            if (buf[11] == 0x03) s_power_values[2] = buf[12]; // Antenna 3 power
            if (buf[13] == 0x04) s_power_values[3] = buf[14]; // Antenna 4 power
            
            s_power_request_pending = 0;
            ESP_LOGI(TAG, "Power response parsed successfully: ant1=%d dBm, ant2=%d dBm, ant3=%d dBm, ant4=%d dBm", 
                     s_power_values[0], s_power_values[1], s_power_values[2], s_power_values[3]);
        } else {
            ESP_LOGW(TAG, "Power response: unexpected data length field: %02X %02X", buf[5], buf[6]);
        }
    } else {
        ESP_LOGD(TAG, "Received data doesn't match power response format (len=%d)", len);
        // Log the received data for debugging
        if (len > 0) {
            char hex_str[256] = "";
            for (size_t i = 0; i < len && i < 20; i++) {
                char temp[4];
                snprintf(temp, sizeof(temp), "%02X ", buf[i]);
                strcat(hex_str, temp);
            }
            ESP_LOGD(TAG, "Received: %s", hex_str);
        }
    }
}

// Parse tag/EPC response from reader (handles multiple tag response formats)
static bool parse_tag_response(const uint8_t *buf, size_t len) {
    // Check for valid frame header: 5A 00 01
    if (len < 9 || buf[0] != 0x5A || buf[1] != 0x00 || buf[2] != 0x01) {
        return false;
    }
    
    uint8_t mid = buf[3]; // Message ID
    
    // Handle different tag response formats
    if (mid == 0x12) {
        // Real-time tag data format (from log): 5A 00 01 12 00 00 LEN [TAG_DATA] CRC CRC
        // Example: 5A 00 01 12 00 00 18 00 0C E2 80 69 15 60 00 02 16 65 10 B3 31 30 00 01 01 FE 08 00 0D F7 32 49 7B
        
        if (len >= 15) { // Minimum for meaningful tag data
            uint16_t data_len = (buf[5] << 8) | buf[6]; // Length field
            
            if (len >= 7 + data_len + 2 && data_len >= 8) { // header + data + CRC, minimum tag data
                // Look for EPC pattern (E2 80 prefix is common for EPC tags)
                const uint8_t *data = &buf[7];
                
                // Find EPC data - typically starts at offset 2-3 in tag data
                for (int epc_start = 2; epc_start <= 4 && epc_start < data_len - 6; epc_start++) {
                    if (data[epc_start] == 0xE2 && data[epc_start + 1] == 0x80) {
                        // Found potential EPC start, determine length
                        int epc_len = 12; // Common EPC-96 length in bytes
                        if (epc_start + epc_len <= data_len) {
                            
                            char epc[128] = "";
                            for (int i = 0; i < epc_len; i++) {
                                char hex_byte[3];
                                snprintf(hex_byte, sizeof(hex_byte), "%02X", data[epc_start + i]);
                                strcat(epc, hex_byte);
                            }
                            
                            // Extract RSSI and antenna from tag data (rough approximation)
                            int rssi = -48; // Default from observed data
                            int ant = 1;    // Default antenna
                            
                            // Try to extract antenna and RSSI from the tag data
                            if (data_len > epc_start + epc_len + 2) {
                                ant = data[epc_start + epc_len + 1]; // Antenna number usually follows EPC
                                if (ant < 1 || ant > 4) ant = 1;   // Validate antenna
                            }
                            
                            // Find or allocate tag slot
                            int idx = find_tag_index(epc);
                            if (idx < 0) idx = alloc_tag_index(epc);
                            
                            if (idx >= 0) {
                                s_tags[idx].rssi = rssi;
                                s_tags[idx].ant = ant;
                                s_tags[idx].last_ms = esp_timer_get_time() / 1000ULL;
                                
                                // Silent operation during high-speed inventory to prevent watchdog timeout
                                // ESP_LOGI(TAG, "TAG[%d] epc=%s rssi=%d ant=%d (%d tags processed)", idx, epc, rssi, ant, tag_log_count);
                                return true;
                            }
                        }
                    }
                }
            }
        }
    } else if (mid == 0x10) {
        // Legacy tag response format: 5A 00 01 10 00 00 LEN [EPC_DATA] CRC CRC
        uint16_t data_len = (buf[5] << 8) | buf[6]; // bytes 5-6 contain length
        
        if (len >= 7 + data_len + 2) { // header + data + CRC
            // Extract EPC data
            char epc[128] = "";
            for (int i = 0; i < data_len && i < 32; i++) { // Limit EPC to reasonable size
                char hex_byte[3];
                snprintf(hex_byte, sizeof(hex_byte), "%02X", buf[7 + i]);
                strcat(epc, hex_byte);
            }
            
            // For now, assume antenna 1 and RSSI -50 (since not in this response format)
            int rssi = -50;
            int ant = 1;
            
            // Find or allocate tag slot
            int idx = find_tag_index(epc);
            if (idx < 0) idx = alloc_tag_index(epc);
            
            if (idx >= 0) {
                s_tags[idx].rssi = rssi;
                s_tags[idx].ant = ant;
                s_tags[idx].last_ms = esp_timer_get_time() / 1000ULL;
                
                // Silent operation to prevent watchdog timeout
                // ESP_LOGI(TAG, "TAG[%d] epc=%s (legacy, count=%d)", idx, epc, legacy_log_count);
                return true;
            }
        }
    }
    
    return false;
}

void rfid_process_bytes(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return;
    
    // During system startup, minimize processing to prevent watchdog timeout
    static int startup_packets = 0;
    startup_packets++;
    
    // Skip heavy processing for first 1000 packets to allow system to stabilize
    if (startup_packets < 1000) {
        if (startup_packets % 100 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return;
    }
    
    // Yield occasionally to prevent watchdog timeout during high-speed processing
    static int process_count = 0;
    if (++process_count % 50 == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // Check for power response if we're waiting for one
    if (s_power_request_pending) {
        parse_power_response(buf, len);
        return; // Don't process as tag data
    }
    
    // Try to parse as tag response (support both MID 0x10 and 0x12)
    if (buf[3] == 0x10 || buf[3] == 0x12) {
        if (parse_tag_response(buf, len)) {
            return; // Successfully parsed as tag response
        }
    }
    
    // Fallback: only process if inventory is running using old method
    if (!s_running) {
        // Don't log every skipped packet to reduce console load
        static int skip_count = 0;
        if (++skip_count % 1000 == 0) {
            ESP_LOGI(TAG, "Inventory not running, skipped %d packets", skip_count);
        }
        return;
    }
    
    // Continue with normal tag processing
    size_t pos = 0;
    int tags_found = 0;
    const int MAX_TAGS_PER_BATCH = 10; // Limit processing per call
    
    while (pos + 6 <= len && tags_found < MAX_TAGS_PER_BATCH) {
        char epc[128]; 
        int rssi = 0, ant = 0; 
        size_t next = pos + 1;
        
        if (extract_one_tag(buf, len, pos, &next, epc, sizeof(epc), &rssi, &ant)) {
            if (epc[0] != '\0') {
                int idx = find_tag_index(epc);
                if (idx < 0) idx = alloc_tag_index(epc);
                
                s_tags[idx].rssi = rssi;
                s_tags[idx].ant = ant;
                s_tags[idx].last_ms = esp_timer_get_time() / 1000ULL;
                
                // Silent operation to prevent watchdog timeout
                // ESP_LOGI(TAG, "TAG[%d] epc=%s rssi=%d ant=%d (fallback, count=%d)", idx, epc, rssi, ant, fallback_log_count);
                tags_found++;
            }
            pos = next;
        } else {
            pos++;
        }
    }
}

int rfid_get_tags_json(char *out, int out_len)
{
    if (!out || out_len <= 10) return 0;
    
    int used = 0;
    used += snprintf(out + used, out_len - used, "[");
    int first = 1;
    int count = 0;
    
    for (int i = 0; i < MAX_TAGS && used < out_len - 50; ++i) {
        if (s_tags[i].epc[0] == '\0') continue;
        
        if (!first) {
            used += snprintf(out + used, out_len - used, ",");
        }
        first = 0;
        
        uint64_t ts = s_tags[i].last_ms;
        used += snprintf(out + used, out_len - used, 
            "{\"epc\":\"%s\",\"rssi\":%d,\"ant\":%d,\"ts\":%llu}",
            s_tags[i].epc, s_tags[i].rssi, s_tags[i].ant, 
            (unsigned long long)ts);
        
        count++;
        if (count >= 50) break; // Limit output size
    }
    
    used += snprintf(out + used, out_len - used, "]");
    return used;
}

void rfid_init(void)
{
    // TODO: initialize actual UFH RFID hardware here
    uart_init(READER_TXD, READER_RXD);
    ESP_LOGI(TAG, "RFID module initialized (stub)");
}

void rfid_start_inventory(void)
{
    if (!s_running) {
        s_running = 1;
        static const uint8_t cmd_start[] = { 0x5A, 0x00, 0x01, 0x02, 0x10, 0x00, 0x05, 0x00,
                                             0x00, 0x00, 0x01, 0x01, 0xF4, 0x87 };
        
        uart_send_bytes((const char*)cmd_start, sizeof(cmd_start));
        ESP_LOGI(TAG, "RFID inventory started");
        // If you have real inventory logic, start a task here
    }
}

void rfid_stop_inventory(void)
{
    if (s_running) {
        s_running = 0;
        // Stop inventory command: 5a000102ff0000885a
        static const uint8_t cmd[] = { 0x5A, 0x00, 0x01, 0x02, 0xFF, 0x00, 0x00, 0x88, 0x5A };
        
        uart_send_bytes((const char*)cmd, sizeof(cmd));
        ESP_LOGI(TAG, "RFID inventory stopped");
        // Stop inventory task if running
    }
}

static uint16_t crc16_xmodem(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint32_t build_pcw(uint8_t category, uint8_t mid, int rs485, int notify) {
    const uint8_t PROTO_TYPE = 0x00;
    const uint8_t PROTO_VER  = 0x01;
    uint32_t pcw = ((uint32_t)PROTO_TYPE << 24) | ((uint32_t)PROTO_VER << 16);
    if (rs485) pcw |= (1u << 13);
    if (notify) pcw |= (1u << 12);
    pcw |= ((uint32_t)category << 8) | mid;
    return pcw;
}

void rfid_set_power(int pwr1, int pwr2, int pwr3, int pwr4)
{
    // Convert power values to uint8_t (0-255 range)
    uint8_t p1 = (uint8_t)(pwr1 & 0xFF);
    uint8_t p2 = (uint8_t)(pwr2 & 0xFF);
    uint8_t p3 = (uint8_t)(pwr3 & 0xFF);
    uint8_t p4 = (uint8_t)(pwr4 & 0xFF);
    
    const uint8_t HEADER = 0x5A;
    const uint16_t MID_CONFIG_POWER = 0x0201;

    uint8_t payload[10] = {
        0x01, p1,
        0x02, p2,
        0x03, p3,
        0x04, p4,
        0xFF, 0x01
    };

    uint8_t frame[1 + 4 + 1 + 2 + sizeof(payload) + 2];
    size_t k = 0;

    frame[k++] = HEADER;

    uint8_t category = (MID_CONFIG_POWER >> 8) & 0xFF;
    uint8_t mid      = MID_CONFIG_POWER & 0xFF;
    uint32_t pcw     = build_pcw(category, mid, 0, 0); // rs485=0, notify=0

    frame[k++] = (uint8_t)((pcw >> 24) & 0xFF);
    frame[k++] = (uint8_t)((pcw >> 16) & 0xFF);
    frame[k++] = (uint8_t)((pcw >> 8)  & 0xFF);
    frame[k++] = (uint8_t)(pcw & 0xFF);

    uint16_t len = (uint16_t)sizeof(payload);
    frame[k++] = (uint8_t)((len >> 8) & 0xFF);
    frame[k++] = (uint8_t)(len & 0xFF);

    memcpy(&frame[k], payload, sizeof(payload));
    k += sizeof(payload);

    uint16_t crc = crc16_xmodem(&frame[1], (size_t)(k - 1));
    frame[k++] = (uint8_t)((crc >> 8) & 0xFF);
    frame[k++] = (uint8_t)(crc & 0xFF);

    uart_send_bytes((const char*)frame, (int)k);
    ESP_LOGI(TAG, "RFID power set to ant1=%d ant2=%d ant3=%d ant4=%d", pwr1, pwr2, pwr3, pwr4);
}


void rfid_get_power(int *pwr1, int *pwr2, int *pwr3, int *pwr4)
{
    // Return current stored power values (without sending query command)
    // Use rfid_query_power() first to refresh values from reader
    if (pwr1) *pwr1 = s_power_values[0];
    if (pwr2) *pwr2 = s_power_values[1];
    if (pwr3) *pwr3 = s_power_values[2];
    if (pwr4) *pwr4 = s_power_values[3];
}

void rfid_query_power(void)
{
    // Send power query command without returning cached values
    // This allows the web interface to trigger a fresh query and then get updated values
    static const uint8_t cmd[] = { 0x5A, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x29, 0x59 };
    s_power_request_pending = 1; // Set flag to indicate we're waiting for power response
    uart_send_bytes((const char*)cmd, sizeof(cmd));
}

// Query reader information (based on NRN SDK MID.QUERY_INFO: 0x0100)
void rfid_query_reader_info(void)
{
    // Command: 5A 00 01 01 00 00 00 [CRC]
    // MID = 0x0100 -> category=0x01, mid=0x00
    static const uint8_t cmd[] = { 0x5A, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x88, 0x5B };
    uart_send_bytes((const char*)cmd, sizeof(cmd));
    printf("Sent reader info query command\n");
}

// Confirm connection (based on NRN SDK MID.CONFIRM_CONNECTION: 0x12)
void rfid_confirm_connection(void)
{
    // Command: 5A 00 01 00 12 00 00 [CRC] 
    // MID = 0x12 -> category=0x00, mid=0x12
    static const uint8_t cmd[] = { 0x5A, 0x00, 0x01, 0x00, 0x12, 0x00, 0x00, 0x29, 0x47 };
    uart_send_bytes((const char*)cmd, sizeof(cmd));
    printf("Sent connection confirmation command\n");
}

const char* rfid_get_status(void)
{
    return s_running ? "running" : "stopped";
}

const char* rfid_get_last_command(void)
{
    return s_last_command;
}

void rfid_set_last_command(const char* cmd_description)
{
    if (cmd_description) {
        strncpy(s_last_command, cmd_description, sizeof(s_last_command) - 1);
        s_last_command[sizeof(s_last_command) - 1] = '\0';
    }
}
