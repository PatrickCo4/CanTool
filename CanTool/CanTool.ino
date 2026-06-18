/*  CanTool.ino  -  ESP32 Mini CAN Communication Adapter
 *  -----------------------------------------------------
 *  Talk to a CAN device through a self-hosted web interface.
 *  ESP32-S3 + SN65HVD230 transceiver + DB9. Hosts a Wi-Fi AP and a
 *  web page that lets you send any CAN frame and watch all bus traffic.
 *
 *  - Send: standard (11-bit) or extended (29-bit) ID, 0-8 data bytes (hex)
 *  - Monitor: live table of every received ID (type, DLC, data, count)
 *  - Bus rate: 125 / 250 / 500 kbit / 1 Mbit, switchable from the page
 *
 *  No external libraries - Arduino-ESP32 core only (WiFi, WebServer, TWAI).
 *
 *  Wiring (change CAN_TX / CAN_RX for your board):
 *    GPIO 1 -> transceiver D/CTX      GPIO 2 -> transceiver R/CRX
 *    3V3 -> VCC,  GND -> GND
 *    transceiver CANH -> DB9 pin 7 (yellow),  CANL -> DB9 pin 2 (green)
 *
 *  Use: join Wi-Fi "CAN-Tool" (pass "cantool123"), open http://192.168.4.1
 */

#include <WiFi.h>
#include <WebServer.h>
#include "webpage.h"   // generated from index.html by gen_webpage.py
#include "driver/twai.h"
#include <ctype.h>
#include <string.h>

static const char*     AP_SSID = "CAN-Tool";
static const char*     AP_PASS = "cantool123";
static const gpio_num_t CAN_TX = GPIO_NUM_1;
static const gpio_num_t CAN_RX = GPIO_NUM_2;

WebServer server(80);
bool      canOk   = false;
int       baudK   = 250;          // current bit rate (kbit)
uint32_t  rxTotal = 0;

struct Seen { uint32_t id; bool ext; uint8_t data[8]; uint8_t dlc; uint32_t count; };
static const int SEEN_MAX = 40;
static Seen seen[SEEN_MAX];
static int  seenCount = 0;

// ---- CAN driver ----
void canStart(int k) {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
  g.rx_queue_len = 64;
  g.tx_queue_len = 16;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  if      (k == 125)  t = TWAI_TIMING_CONFIG_125KBITS();
  else if (k == 500)  t = TWAI_TIMING_CONFIG_500KBITS();
  else if (k == 1000) t = TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  canOk = (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK);
  baudK = k;
}

void canRestart(int k) {
  if (canOk) { twai_stop(); twai_driver_uninstall(); canOk = false; }
  seenCount = 0; rxTotal = 0;
  canStart(k);
}

void drain() {
  if (!canOk) return;
  twai_message_t m;
  int burst = 0;
  while (burst++ < 40) {
    if (twai_receive(&m, 0) != ESP_OK) break;
    rxTotal++;
    int dlc = m.data_length_code; if (dlc > 8) dlc = 8;
    Seen* s = nullptr;
    for (int i = 0; i < seenCount; i++)
      if (seen[i].id == m.identifier && seen[i].ext == (bool)m.extd) { s = &seen[i]; break; }
    if (!s && seenCount < SEEN_MAX) { s = &seen[seenCount++]; s->count = 0; }
    if (s) { s->id = m.identifier; s->ext = m.extd; s->dlc = dlc; memcpy(s->data, m.data, dlc); s->count++; }
  }
}

// ---- helpers ----
int parseHexBytes(const String& in, uint8_t* out, int maxN) {
  String h;
  for (size_t i = 0; i < in.length(); i++) if (isxdigit((int)in[i])) h += in[i];
  int n = 0;
  for (int i = 0; i + 1 < (int)h.length() && n < maxN; i += 2)
    out[n++] = (uint8_t)strtoul(h.substring(i, i + 2).c_str(), nullptr, 16);
  return n;
}

// ---- web page is embedded as a gzip blob in webpage.h (run gen_webpage.py to rebuild)
// ---- handlers ----
void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)webpage_gz, webpage_gz_len);
}

void handleRx() {
  String o = "{\"baud\":" + String(baudK) + ",\"total\":" + String(rxTotal) + ",\"frames\":[";
  for (int i = 0; i < seenCount; i++) {
    if (i) o += ",";
    char idbuf[12];
    sprintf(idbuf, seen[i].ext ? "%08lX" : "%03lX", (unsigned long)seen[i].id);
    char hx[24]; char* p = hx;
    for (int b = 0; b < seen[i].dlc; b++) p += sprintf(p, b ? " %02X" : "%02X", seen[i].data[b]);
    o += "{\"id\":\""; o += idbuf;
    o += "\",\"ext\":"; o += (seen[i].ext ? "1" : "0");
    o += ",\"dlc\":" + String(seen[i].dlc);
    o += ",\"n\":" + String(seen[i].count);
    o += ",\"data\":\""; o += hx; o += "\"}";
  }
  o += "]}";
  server.send(200, "application/json", o);
}

void handleTx() {
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 16);
  bool ext = (server.arg("ext") == "1");
  uint8_t d[8];
  int n = parseHexBytes(server.arg("data"), d, 8);
  if (!canOk) { server.send(200, "text/plain", "CAN not started"); return; }
  twai_message_t m = {};
  m.identifier = id;
  m.extd = ext ? 1 : 0;
  m.data_length_code = n;
  for (int i = 0; i < n; i++) m.data[i] = d[i];
  esp_err_t r = twai_transmit(&m, pdMS_TO_TICKS(10));
  server.send(200, "text/plain", r == ESP_OK ? "sent" : "tx failed");
}

void handleBaud() {
  int k = server.arg("k").toInt();
  if (k != 125 && k != 250 && k != 500 && k != 1000) k = 250;
  canRestart(k);
  server.send(200, "text/plain", String("bus set to ") + k + " kbit");
}

void handleClear() { seenCount = 0; rxTotal = 0; server.send(200, "text/plain", "cleared"); }

void setup() {
  Serial.begin(115200);
  canStart(250);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP \""); Serial.print(AP_SSID);
  Serial.print("\" -> http://"); Serial.println(WiFi.softAPIP());
  server.on("/",      HTTP_GET,  handleRoot);
  server.on("/rx",    HTTP_GET,  handleRx);
  server.on("/tx",    HTTP_POST, handleTx);
  server.on("/baud",  HTTP_POST, handleBaud);
  server.on("/clear", HTTP_POST, handleClear);
  server.begin();
}

void loop() {
  drain();
  server.handleClient();
}
