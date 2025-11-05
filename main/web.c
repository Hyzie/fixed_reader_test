#include "web.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "uart.h"
#include "wifi_config.h"
#include "rfid.h"
#include <stdlib.h>

static const char *TAG = "WEB";

// --- Giao diện Web (HTML, CSS, JS) ---
static const char* HTML_CONTENT = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 UHF RFID Config</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; background:#222; color:#eee; }
    .container { max-width:900px; margin:20px auto; padding:20px; background:#2b2f33; border-radius:8px; }
    h1 { text-align:center }
    label { display:block; margin-top:10px }
    input[type=text], input[type=password] { width:100%; padding:8px; margin-top:4px; box-sizing:border-box; background:#1e2226; color:#eee; border:1px solid #444 }
    button { margin-top:10px; padding:10px 14px; background:#4caf50; color:#fff; border:none; border-radius:4px; cursor:pointer }
    .danger { background:#e53935 }
    .row { display:flex; gap:10px }
    .col { flex:1 }
    pre { background:#111; padding:10px; height:200px; overflow:auto }
  </style>
</head>
<body>
  <div class="container">
    <h1>UHF RFID - Ethernet Config</h1>

    <h3>WiFi Configuration</h3>
    <form id="wifiForm" onsubmit="saveWifi(event)">
      <label>SSID
        <input type="text" id="ssid" placeholder="WiFi SSID">
      </label>
      <label>Password
        <input type="password" id="pass" placeholder="WiFi Password">
      </label>
      <button type="submit">Save WiFi</button>
    </form>

    <h3>RFID Controls</h3>
    <div class="row">
      <div class="col"><button onclick="startInv()">Start Inventory</button></div>
      <div class="col"><button class="danger" onclick="stopInv()">Stop Inventory</button></div>
    </div>

    <h3>Power Control</h3>
    <div class="row">
      <div class="col">
        <label>Antenna 1 Power (dBm)
          <input type="text" id="pwr1" placeholder="30">
        </label>
      </div>
      <div class="col">
        <label>Antenna 2 Power (dBm)
          <input type="text" id="pwr2" placeholder="30">
        </label>
      </div>
    </div>
    <div class="row">
      <div class="col">
        <label>Antenna 3 Power (dBm)
          <input type="text" id="pwr3" placeholder="30">
        </label>
      </div>
      <div class="col">
        <label>Antenna 4 Power (dBm)
          <input type="text" id="pwr4" placeholder="30">
        </label>
      </div>
    </div>
    <div class="row">
      <div class="col"><button onclick="setPower()">Set Power</button></div>
      <div class="col"><button onclick="getPower()">Get Power</button></div>
    </div>

    <h3>Status</h3>
    <pre id="status">Loading...</pre>

  <h3>Tags</h3>
  <pre id="tags">Loading tags...</pre>

    <h3>UART Terminal (Hex Data)</h3>
    <div class="row">
      <div class="col"><button onclick="clearTerminal()">Clear Terminal</button></div>
    </div>
    <pre id="terminal" style="font-family: monospace; font-size: 12px; background: #000; color: #0f0; padding: 10px; height: 300px; overflow-y: auto;"></pre>
    <form onsubmit="sendMessage(event)">
      <input type="text" id="message" placeholder="Enter hex message (e.g. 5A 00 01 02 02 00 00 29 59)">
      <button type="submit">Send</button>
    </form>
  </div>

  <script>
    async function fetchStatus(){
      try{
        const r = await fetch('/status');
        const json = await r.json();
        let statusText = `Inventory: ${json.inventory}\n`;
        statusText += `Last Command: ${json.last_command}\n`;
        statusText += `WiFi: ${json.wifi.configured ? 'Configured' : 'Not configured'}`;
        if (json.wifi.configured) {
          statusText += ` (${json.wifi.ssid})`;
        }
        document.getElementById('status').textContent = statusText;
      }catch(e){ 
        document.getElementById('status').textContent = 'Error fetching status'; 
      }
    }

    // Poll status every 1s
    setInterval(fetchStatus, 1000);
    fetchStatus();

    // Poll tags every 800ms
    async function fetchTags(){
      try{
        const r = await fetch('/tags');
        if (!r.ok) return;
        const j = await r.json();
        const el = document.getElementById('tags');
        el.textContent = '';
        for (let i=0;i<j.length;i++){
          const t = j[i];
          el.textContent += `epc=${t.epc} rssi=${t.rssi} ant=${t.ant} ts=${t.ts}\n`;
        }
      }catch(e){ }
    }
    setInterval(fetchTags, 800);
    fetchTags();

    // Terminal polling
    async function pollTerminal(){
      try{
        const r = await fetch('/data');
        const t = await r.text();
        if (t.length>0){
          const term = document.getElementById('terminal');
          // Add timestamp for each data chunk
          const now = new Date().toLocaleTimeString();
          term.textContent += `[${now}] RX: ${t}\n`;
          term.scrollTop = term.scrollHeight;
        }
      }catch(e){ }
    }
    setInterval(pollTerminal, 500);

    function clearTerminal(){
      const term = document.getElementById('terminal');
      term.textContent = '';
    }

    async function sendMessage(e){
      e.preventDefault();
      const msg = document.getElementById('message').value;
      if (!msg) return;
      
      // Parse hex input (allow spaces and convert to bytes)
      const hexBytes = msg.split(/\s+/).filter(h => h.length > 0);
      let hexString = '';
      for (let hex of hexBytes) {
        if (hex.length === 2 && /^[0-9A-Fa-f]{2}$/.test(hex)) {
          hexString += hex.toUpperCase() + ' ';
        }
      }
      
      await fetch('/send', { method:'POST', headers:{'Content-Type':'text/plain'}, body: msg });
      const term = document.getElementById('terminal');
      const now = new Date().toLocaleTimeString();
      term.textContent += `[${now}] TX: ${hexString || msg}\n`;
      document.getElementById('message').value='';
      term.scrollTop = term.scrollHeight;
    }

    async function saveWifi(e){
      e.preventDefault();
      const ssid = encodeURIComponent(document.getElementById('ssid').value);
      const pass = encodeURIComponent(document.getElementById('pass').value);
      const body = `ssid=${ssid}&pass=${pass}`;
      await fetch('/wifi-config', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body });
      fetchStatus();
    }

    async function startInv(){ await fetch('/inventory/start', { method:'POST' }); fetchStatus(); }
    async function stopInv(){ await fetch('/inventory/stop', { method:'POST' }); fetchStatus(); }

    async function setPower(){
      const pwr1 = document.getElementById('pwr1').value || '30';
      const pwr2 = document.getElementById('pwr2').value || '30';
      const pwr3 = document.getElementById('pwr3').value || '30';
      const pwr4 = document.getElementById('pwr4').value || '30';
      const body = `pwr1=${pwr1}&pwr2=${pwr2}&pwr3=${pwr3}&pwr4=${pwr4}`;
      await fetch('/power/set', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body });
      alert('Power settings sent');
    }

    async function getPower(){
      try{
        const r = await fetch('/power/get');
        const json = await r.json();
        if (json) {
          document.getElementById('pwr1').value = json.pwr1 || '30';
          document.getElementById('pwr2').value = json.pwr2 || '30';
          document.getElementById('pwr3').value = json.pwr3 || '30';
          document.getElementById('pwr4').value = json.pwr4 || '30';
          
          // Show current power values to user
          alert(`Current Power: Ant1=${json.pwr1}dBm, Ant2=${json.pwr2}dBm, Ant3=${json.pwr3}dBm, Ant4=${json.pwr4}dBm`);
        }
      }catch(e){ alert('Error getting power settings'); }
    }

    async function initForm(){
      try{
        const r = await fetch('/status');
        const json = await r.json();
        if (json && json.wifi) {
          if (json.wifi.ssid) document.getElementById('ssid').value = json.wifi.ssid;
          if (json.wifi.pass) document.getElementById('pass').value = json.wifi.pass;
        }
      }catch(e){}
      // Load current power settings
      getPower();
    }
    initForm();
  </script>
</body>
</html>
)rawliteral";

// HTTP GET handler - serve main page
static esp_err_t http_get_handler(httpd_req_t *req)
{
    return httpd_resp_send(req, HTML_CONTENT, HTTPD_RESP_USE_STRLEN);
}

// HTTP GET handler - serve favicon (simple 1x1 transparent PNG to avoid 404)
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    // Simple 1x1 transparent PNG (67 bytes)
    static const uint8_t favicon_ico[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x37, 0x6E, 0xF9, 0x24, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x62, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x01, 0xE2, 0x21, 0xBC, 0x33, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
    
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char*)favicon_ico, sizeof(favicon_ico));
}

// HTTP GET handler - get UART data
static esp_err_t data_get_handler(httpd_req_t *req)
{
  /* Use heap for the response buffer to keep httpd task stack usage small */
  char *response = (char*) malloc(1024);
  if (!response) return ESP_ERR_HTTPD_ALLOC_MEM;
  memset(response, 0, 1024);
  int len = uart_get_rx_data(response, 1024);
  (void)len;
  httpd_resp_set_type(req, "text/plain");
  esp_err_t err = httpd_resp_send(req, response, strlen(response));
  free(response);
  return err;
}

// Utility: URL-decode in-place
static void urldecode(char *dst, const char *src)
{
  char a, b;
  while (*src) {
    if (*src == '%') {
      a = src[1]; b = src[2];
      if (a && b) {
        if (a >= 'a') a -= 'a' - 'A';
        if (a >= 'A') a = a - 'A' + 10; else a -= '0';
        if (b >= 'a') b -= 'a' - 'A';
        if (b >= 'A') b = b - 'A' + 10; else b -= '0';
        *dst++ = (char)(16 * a + b);
        src += 3;
        continue;
      }
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
      continue;
    }
    *dst++ = *src++;
  }
  *dst = '\0';
}

// HTTP POST handler - save Wi-Fi config (form urlencoded: ssid=..&pass=..)
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  char *pair = strtok(buf, "&");
  char ssid[64] = {0}, pass[64] = {0};
  while (pair) {
    char *eq = strchr(pair, '=');
    if (eq) {
      *eq = '\0';
      char *k = pair; char *v = eq + 1;
      char dec[128];
      urldecode(dec, v);
      if (strcmp(k, "ssid") == 0) strncpy(ssid, dec, sizeof(ssid)-1);
      else if (strcmp(k, "pass") == 0) strncpy(pass, dec, sizeof(pass)-1);
    }
    pair = strtok(NULL, "&");
  }
  wifi_config_save(ssid, pass);
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

// Inventory control handlers
static esp_err_t inventory_start_handler(httpd_req_t *req)
{
  rfid_start_inventory();
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t inventory_stop_handler(httpd_req_t *req)
{
  rfid_stop_inventory();
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

// Status handler returns JSON with wifi and inventory status
static esp_err_t status_get_handler(httpd_req_t *req)
{
  char ssid[64]; char pass[64];
  wifi_config_load(ssid, sizeof(ssid), pass, sizeof(pass));
  const char *inv = rfid_get_status();
  const char *last_cmd = rfid_get_last_command();
  char resp[1024];
  int configured = (ssid[0] != '\0');
  int len = snprintf(resp, sizeof(resp), 
    "{\"inventory\":\"%s\",\"last_command\":\"%s\",\"wifi\":{\"configured\":%d,\"ssid\":\"%s\",\"pass\":\"%s\"}}", 
    inv, last_cmd, configured, ssid, pass);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, resp, len);
}

// Tags endpoint
static esp_err_t tags_get_handler(httpd_req_t *req)
{
  /* Allocate tag JSON buffer on heap to avoid large stack usage in httpd task */
  char *buf = (char*) malloc(2048);
  if (!buf) { httpd_resp_send(req, "[]", 2); return ESP_ERR_HTTPD_ALLOC_MEM; }
  int used = rfid_get_tags_json(buf, 2048);
  if (used <= 0) { free(buf); httpd_resp_send(req, "[]", 2); return ESP_OK; }
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_send(req, buf, used);
  free(buf);
  return err;
}

// Power control handlers
static esp_err_t power_set_handler(httpd_req_t *req)
{
  char buf[256];
  int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  
  int pwr1 = 30, pwr2 = 30, pwr3 = 30, pwr4 = 30;
  char *pair = strtok(buf, "&");
  while (pair) {
    char *eq = strchr(pair, '=');
    if (eq) {
      *eq = '\0';
      char *k = pair; char *v = eq + 1;
      char dec[32];
      urldecode(dec, v);
      if (strcmp(k, "pwr1") == 0) pwr1 = atoi(dec);
      else if (strcmp(k, "pwr2") == 0) pwr2 = atoi(dec);
      else if (strcmp(k, "pwr3") == 0) pwr3 = atoi(dec);
      else if (strcmp(k, "pwr4") == 0) pwr4 = atoi(dec);
    }
    pair = strtok(NULL, "&");
  }
  
  // Call the fixed function signature
  rfid_set_power(pwr1, pwr2, pwr3, pwr4);
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t power_get_handler(httpd_req_t *req)
{
  // First trigger a fresh power query to get current values from reader
  rfid_query_power();
  
  // Wait a moment for the reader to respond and update the stored values
  vTaskDelay(pdMS_TO_TICKS(300));
  
  // Now get the updated power values
  int pwr1, pwr2, pwr3, pwr4;
  rfid_get_power(&pwr1, &pwr2, &pwr3, &pwr4);
  
  char resp[256];
  int len = snprintf(resp, sizeof(resp), 
    "{\"pwr1\":%d,\"pwr2\":%d,\"pwr3\":%d,\"pwr4\":%d}", 
    pwr1, pwr2, pwr3, pwr4);
  
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, resp, len);
}

// HTTP POST handler - send data to UART
static esp_err_t send_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content));

    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    // Send to UART via API
    uart_send_bytes(content, (size_t)ret);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
   httpd_config_t config = HTTPD_DEFAULT_CONFIG();
   /* Increase server task stack to avoid stack overflows in handlers that
     use moderately large buffers (JSON, HTML). Default is 4096. */
   config.stack_size = 8192;
   config.lru_purge_enable = true;
   /* Increase max URI handlers from default (8) to accommodate all endpoints */
   config.max_uri_handlers = 13;  // Increased to 13 for favicon handler
   httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Root page
        const httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = http_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root);

        // Favicon handler
        const httpd_uri_t favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &favicon);

        // Get data endpoint
        const httpd_uri_t data = {
            .uri       = "/data",
            .method    = HTTP_GET,
            .handler   = data_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &data);

    // Send data endpoint
        const httpd_uri_t send = {
            .uri       = "/send",
            .method    = HTTP_POST,
            .handler   = send_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &send);

    // Wi-Fi config endpoint
    const httpd_uri_t wifi_cfg = {
      .uri       = "/wifi-config",
      .method    = HTTP_POST,
      .handler   = wifi_post_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &wifi_cfg);

    // Inventory control endpoints
    const httpd_uri_t inv_start = {
      .uri       = "/inventory/start",
      .method    = HTTP_POST,
      .handler   = inventory_start_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &inv_start);

    const httpd_uri_t inv_stop = {
      .uri       = "/inventory/stop",
      .method    = HTTP_POST,
      .handler   = inventory_stop_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &inv_stop);

    // Status endpoint
    const httpd_uri_t status = {
      .uri       = "/status",
      .method    = HTTP_GET,
      .handler   = status_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &status);

    // Tags endpoint
    const httpd_uri_t tags = {
      .uri       = "/tags",
      .method    = HTTP_GET,
      .handler   = tags_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &tags);

    // Power control endpoints
    const httpd_uri_t power_set = {
      .uri       = "/power/set",
      .method    = HTTP_POST,
      .handler   = power_set_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &power_set);
    ESP_LOGI(TAG, "Registered /power/set handler");

    const httpd_uri_t power_get = {
      .uri       = "/power/get",
      .method    = HTTP_GET,
      .handler   = power_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &power_get);
    ESP_LOGI(TAG, "Registered /power/get handler");
    }
    return server;
}
