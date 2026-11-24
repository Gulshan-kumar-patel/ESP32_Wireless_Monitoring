/*
 * ESP32_Wireless_Monitoring - TRANSMITTER (TX)
 *
 * Reads a 10 k potentiometer and a push button, creates the Wi-Fi hotspot
 * "esp_tx", and serves live data as HTTP + JSON plus a small dashboard.
 *
 *   GET /            dashboard (tx_dashboard.h)
 *   GET /api/data    sensor JSON, polled by the receiver
 *   GET /api/status  access point / server status
 *
 * Target : classic ESP32 (ESP32-WROOM-32 / DevKit V1), Arduino-ESP32 core 3.x
 * Built  : core 3.3.12, Adafruit GFX 2.0.0, Adafruit SSD1306 2.5.17,
 *          Adafruit BusIO 1.17.4
 *
 * Task layout (nothing here waits on anything else):
 *   loop()      core 1, prio 1 - HTTP server only
 *   sampleTask  core 1, prio 3 - button + ADC, fixed rate
 *   liveTask    core 0, prio 2 - UDP stream of every sample to the receiver
 *   displayTask core 0, prio 1 - OLED (the only user of the I2C bus)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "tx_dashboard.h"

// ============================================================================
//  CONFIGURATION
// ============================================================================

// --- Debug ------------------------------------------------------------------
#define DEBUG_SERIAL 1                          // 0 removes all serial output
constexpr uint32_t SERIAL_BAUD        = 115200;
constexpr uint32_t DEBUG_INTERVAL_MS  = 5000;

// --- GPIO (authoritative pin map, see docs/PINOUT.md) -----------------------
constexpr uint8_t PIN_OLED_SDA = 21;
constexpr uint8_t PIN_OLED_SCL = 22;
constexpr uint8_t PIN_POT      = 34;            // ADC1_CH6, input only, no pulls
constexpr uint8_t PIN_BUTTON   = 27;            // to GND, INPUT_PULLUP, pressed = LOW

// --- Wi-Fi access point -----------------------------------------------------
constexpr char    AP_SSID[]          = "esp_tx";
constexpr char    AP_PASSWORD[]      = "esp32@2027";
constexpr uint8_t AP_CHANNEL         = 6;
constexpr uint8_t AP_MAX_CLIENTS     = 6;       // RX + phones/laptops
constexpr uint32_t AP_RETRY_MS       = 5000;    // if the AP fails to start
const IPAddress   AP_IP(192, 168, 4, 1);
const IPAddress   AP_MASK(255, 255, 255, 0);

// --- HTTP server ------------------------------------------------------------
constexpr uint16_t HTTP_PORT         = 80;
// A browser may open a socket and send nothing. The stock WebServer waits 5 s
// for it while nobody else is served. These limits keep the RX link alive.
constexpr uint32_t HTTP_IDLE_BUSY_MS = 40;      // idle limit when another client waits
constexpr uint32_t HTTP_IDLE_MAX_MS  = 1000;    // idle limit otherwise

// --- Live stream (UDP) ------------------------------------------------------
// The receiver asks for the stream, then gets every sample as it is taken.
constexpr uint16_t LIVE_PORT         = 4210;    // must match the receiver
constexpr char     LIVE_SUBSCRIBE[]  = "SUB";   // must match the receiver
constexpr uint32_t LIVE_LEASE_MS     = 2000;    // stop if not asked again by then
constexpr uint32_t LIVE_TICK_MS      = 5;

// --- Sampling ---------------------------------------------------------------
constexpr uint32_t SAMPLE_INTERVAL_MS   = 20;   // ADC + packet rate (target 20-50)
constexpr uint32_t BUTTON_TICK_MS       = 5;    // must divide SAMPLE_INTERVAL_MS
constexpr uint32_t BUTTON_DEBOUNCE_MS   = 30;
constexpr uint8_t  ADC_OVERSAMPLE       = 8;    // readings averaged per sample
constexpr uint8_t  ADC_SMOOTH_SHIFT     = 1;    // EMA weight 1/2 per sample, 0 = off
constexpr int32_t  POT_DEADBAND_RAW     = 8;    // ignore jitter smaller than this
constexpr int32_t  POT_RAW_MIN          = 0;    // raw value shown as 0 %
constexpr int32_t  POT_RAW_MAX          = 4095; // raw value shown as 100 %
constexpr uint32_t SAMPLE_STALL_MS      = 500;  // status becomes FAULT beyond this

// --- OLED -------------------------------------------------------------------
constexpr uint8_t  OLED_ADDRESS      = 0x3C;    // some modules use 0x3D
constexpr uint32_t OLED_I2C_HZ       = 400000;
constexpr uint32_t OLED_INTERVAL_MS  = 200;     // redrawn only when text changed
constexpr uint32_t OLED_RETRY_MS     = 5000;    // if the module is not found

// ============================================================================
//  COMPILE-TIME CHECKS
// ============================================================================

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "This pin map is for the classic ESP32 only. Select 'ESP32 Dev Module' or 'DOIT ESP32 DEVKIT V1'."
#endif
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Arduino-ESP32 core 3.x is required (built with 3.3.12)."
#endif

// A macro, not a function: the Arduino builder inserts prototypes before the
// first function, which must come after the types below.
#define PIN_IS_SAFE(p) (!((p) >= 6 && (p) <= 11) && (p) != 12)
static_assert(PIN_IS_SAFE(PIN_OLED_SDA) && PIN_IS_SAFE(PIN_OLED_SCL) &&
              PIN_IS_SAFE(PIN_POT) && PIN_IS_SAFE(PIN_BUTTON),
              "GPIO6-11 (flash) and GPIO12 (boot strap) must not be used");
static_assert(SAMPLE_INTERVAL_MS % BUTTON_TICK_MS == 0,
              "BUTTON_TICK_MS must divide SAMPLE_INTERVAL_MS");
static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "WPA2 needs at least 8 characters");
static_assert(POT_RAW_MAX > POT_RAW_MIN, "POT_RAW_MAX must exceed POT_RAW_MIN");

#if DEBUG_SERIAL
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

// ============================================================================
//  TYPES AND SHARED STATE
// ============================================================================

struct SensorData {
  uint16_t potRaw;
  uint8_t  potPercent;
  uint8_t  switchOn;       // 1 = pressed / ON, 0 = released / OFF
  uint32_t sequence;       // +1 per sample; RX uses it to detect a frozen TX
  uint32_t sampleMs;
};

static portMUX_TYPE  dataMux = portMUX_INITIALIZER_UNLOCKED;
static SensorData    sensor  = {};
static volatile bool apRunning     = false;
static volatile bool serverRunning = false;
static volatile bool oledOk        = false;
static volatile uint8_t apClients  = 0;

// WebServer that does not let one silent socket block everybody else.
class ResponsiveWebServer : public WebServer {
 public:
  explicit ResponsiveWebServer(uint16_t port) : WebServer(port) {}

  void handleClient() override {
    if (_currentStatus == HC_WAIT_READ && !_currentClient.available()) {
      const uint32_t idle = millis() - _statusChange;
      if (idle > HTTP_IDLE_MAX_MS || (idle > HTTP_IDLE_BUSY_MS && _server.hasClient())) {
        _currentClient = NetworkClient();       // closes the idle socket
        _currentStatus = HC_NONE;
      }
    }
    WebServer::handleClient();
  }
};

static ResponsiveWebServer server(HTTP_PORT);
static Adafruit_SSD1306 oled(128, 64, &Wire, -1, OLED_I2C_HZ, OLED_I2C_HZ);

// ============================================================================
//  SAMPLING
// ============================================================================

// Maps a raw reading to 0..100 with rounding and clamping.
static uint8_t rawToPercent(int32_t raw) {
  if (raw <= POT_RAW_MIN) return 0;
  if (raw >= POT_RAW_MAX) return 100;
  const int32_t span = POT_RAW_MAX - POT_RAW_MIN;
  return (uint8_t)(((raw - POT_RAW_MIN) * 100 + span / 2) / span);
}

static int32_t readPotAveraged() {
  int32_t sum = 0;
  for (uint8_t i = 0; i < ADC_OVERSAMPLE; i++) sum += analogRead(PIN_POT);
  return (sum + ADC_OVERSAMPLE / 2) / ADC_OVERSAMPLE;
}

static void sampleTask(void *) {
  bool stablePressed = digitalRead(PIN_BUTTON) == LOW;
  bool lastPressed   = stablePressed;
  uint32_t lastEdgeMs = millis();

  int32_t emaX16    = readPotAveraged() * 16;   // fixed point, 4 fraction bits
  int32_t published = emaX16 / 16;
  uint32_t tick = 0;
  const uint32_t ticksPerSample = SAMPLE_INTERVAL_MS / BUTTON_TICK_MS;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();

    const bool pressed = digitalRead(PIN_BUTTON) == LOW;
    if (pressed != lastPressed) {
      lastPressed = pressed;
      lastEdgeMs  = now;
    } else if (pressed != stablePressed && now - lastEdgeMs >= BUTTON_DEBOUNCE_MS) {
      stablePressed = pressed;
    }

    if (tick++ % ticksPerSample == 0) {
      emaX16 += (readPotAveraged() * 16 - emaX16) / (1 << ADC_SMOOTH_SHIFT);
      const int32_t smooth = (emaX16 + 8) / 16;
      if (abs(smooth - published) >= POT_DEADBAND_RAW) published = smooth;
      published = constrain(published, (int32_t)0, (int32_t)4095);
      const uint8_t percent = rawToPercent(published);

      portENTER_CRITICAL(&dataMux);
      sensor.potRaw     = (uint16_t)published;
      sensor.potPercent = percent;
      sensor.switchOn   = stablePressed ? 1 : 0;
      sensor.sequence++;
      sensor.sampleMs   = now;
      portEXIT_CRITICAL(&dataMux);
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(BUTTON_TICK_MS));
  }
}

static SensorData readSensor() {
  SensorData copy;
  portENTER_CRITICAL(&dataMux);
  copy = sensor;
  portEXIT_CRITICAL(&dataMux);
  return copy;
}

// FAULT means the sampling task stopped: the receiver rejects such packets.
static const char *deviceStatus(const SensorData &d) {
  const bool fresh = d.sequence != 0 && (millis() - d.sampleMs) < SAMPLE_STALL_MS;
  return fresh ? "ONLINE" : "FAULT";
}

// ============================================================================
//  OLED
// ============================================================================

constexpr uint8_t OLED_ROWS = 8;
constexpr uint8_t OLED_COLS = 21;
static char oledNew[OLED_ROWS][OLED_COLS + 1];
static char oledOld[OLED_ROWS][OLED_COLS + 1];

static bool oledPresent() {
  Wire.beginTransmission(OLED_ADDRESS);
  return Wire.endTransmission() == 0;
}

static void oledCompose() {
  const SensorData d = readSensor();
  const IPAddress ip = WiFi.softAPIP();
  snprintf(oledNew[0], sizeof(oledNew[0]), "ESP32 TRANSMITTER");
  snprintf(oledNew[1], sizeof(oledNew[1]), "AP:%s  Clients:%u", apRunning ? "ON" : "OFF", apClients);
  snprintf(oledNew[2], sizeof(oledNew[2]), "SSID:%s", AP_SSID);
  snprintf(oledNew[3], sizeof(oledNew[3]), "IP:%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  snprintf(oledNew[4], sizeof(oledNew[4]), "Pot raw:%u", d.potRaw);
  snprintf(oledNew[5], sizeof(oledNew[5]), "Pot:%u%%", d.potPercent);
  snprintf(oledNew[6], sizeof(oledNew[6]), "Button:%s", d.switchOn ? "PRESSED/ON" : "RELEASED/OFF");
  snprintf(oledNew[7], sizeof(oledNew[7]), "HTTP:%s %s", serverRunning ? "RUNNING" : "STOPPED", deviceStatus(d));
}

static void displayTask(void *) {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_I2C_HZ);
  for (;;) {
    if (!oledPresent()) {
      if (oledOk) DBG("[OLED] lost\n");
      oledOk = false;
      vTaskDelay(pdMS_TO_TICKS(OLED_RETRY_MS));
      continue;
    }
    if (!oledOk) {
      // Last argument false: the bus is already started with our pins.
      oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, false, false);
      DBG("[OLED] init %s\n", oledOk ? "ok" : "failed");
      if (!oledOk) {
        vTaskDelay(pdMS_TO_TICKS(OLED_RETRY_MS));
        continue;
      }
      memset(oledOld, 0, sizeof(oledOld));
    }

    memset(oledNew, 0, sizeof(oledNew));
    oledCompose();
    if (memcmp(oledNew, oledOld, sizeof(oledNew)) != 0) {
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setTextWrap(false);
      for (uint8_t r = 0; r < OLED_ROWS; r++) {
        oled.setCursor(0, r * 8);
        oled.print(oledNew[r]);
      }
      oled.display();
      memcpy(oledOld, oledNew, sizeof(oledOld));
    }
    vTaskDelay(pdMS_TO_TICKS(OLED_INTERVAL_MS));
  }
}

// ============================================================================
//  HTTP
// ============================================================================

static char jsonBuf[384];                       // used only from loop()

static void sendJson(int len) {
  if (len < 0 || len >= (int)sizeof(jsonBuf)) {
    server.send(500, "application/json", "{\"error\":\"BUFFER\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "application/json", jsonBuf, (size_t)len);
}

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", TX_DASHBOARD_HTML, TX_DASHBOARD_HTML_LEN);
}

// Sensor JSON, the same for /api/data and the live stream.
static int formatData(char *buf, size_t size, const SensorData &d) {
  return snprintf(buf, size,
    "{\"device\":\"ESP32_TX\",\"pot_raw\":%u,\"pot_percent\":%u,\"switch\":%u,"
    "\"wifi_clients\":%u,\"uptime_ms\":%lu,\"sequence\":%lu,\"status\":\"%s\"}",
    d.potRaw, d.potPercent, d.switchOn, apClients,
    (unsigned long)millis(), (unsigned long)d.sequence, deviceStatus(d));
}

static void handleData() {
  sendJson(formatData(jsonBuf, sizeof(jsonBuf), readSensor()));
}

static void handleStatus() {
  const SensorData d = readSensor();
  const IPAddress ip = WiFi.softAPIP();
  sendJson(snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"device\":\"ESP32_TX\",\"status\":\"%s\",\"ap_state\":\"%s\",\"ssid\":\"%s\","
    "\"ip\":\"%u.%u.%u.%u\",\"wifi_clients\":%u,\"uptime_ms\":%lu,\"sequence\":%lu,"
    "\"server_state\":\"%s\",\"oled_state\":\"%s\",\"free_heap\":%lu}",
    deviceStatus(d), apRunning ? "ON" : "OFF", AP_SSID,
    ip[0], ip[1], ip[2], ip[3], apClients,
    (unsigned long)millis(), (unsigned long)d.sequence,
    serverRunning ? "RUNNING" : "STOPPED", oledOk ? "OK" : "NOT_FOUND",
    (unsigned long)ESP.getFreeHeap()));
}

static void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"NOT_FOUND\"}");
}

// ============================================================================
//  LIVE STREAM (liveTask only)
// ============================================================================

static void liveTask(void *) {
  static WiFiUDP udp;
  static char buf[256];
  bool      listening = false;
  bool      active    = false;                  // a receiver asked for the stream
  IPAddress peerIp;
  uint16_t  peerPort  = 0;
  uint32_t  leaseMs   = 0;
  uint32_t  lastSeq   = 0;

  for (;;) {
    if (!listening) listening = apRunning && udp.begin(LIVE_PORT);

    if (listening && udp.parsePacket() > 0) {
      char text[8] = "";
      udp.read(text, sizeof(text) - 1);
      udp.clear();                              // drop the rest of a longer message
      if (strcmp(text, LIVE_SUBSCRIBE) == 0) {
        peerIp   = udp.remoteIP();
        peerPort = udp.remotePort();
        leaseMs  = millis();
        active   = true;
      }
    }
    if (active && millis() - leaseMs > LIVE_LEASE_MS) active = false;

    if (active) {
      const SensorData d = readSensor();
      if (d.sequence != lastSeq) {
        lastSeq = d.sequence;
        const int len = formatData(buf, sizeof(buf), d);
        if (len > 0 && len < (int)sizeof(buf) && udp.beginPacket(peerIp, peerPort)) {
          udp.write((const uint8_t *)buf, (size_t)len);
          udp.endPacket();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(LIVE_TICK_MS));
  }
}

// ============================================================================
//  WI-FI ACCESS POINT
// ============================================================================

static void startAccessPoint() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);                         // lowest latency
  const bool cfgOk = WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  const bool apOk  = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  apRunning = cfgOk && apOk;
  DBG("[AP] %s ssid=%s ip=%s\n", apRunning ? "started" : "FAILED",
      AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
#if DEBUG_SERIAL
  Serial.begin(SERIAL_BAUD);
#endif
  DBG("\n[TX] boot\n");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_POT, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);   // full 0-3.3 V span (approx.)

  xTaskCreatePinnedToCore(sampleTask, "sample", 3072, nullptr, 3, nullptr, 1);

  startAccessPoint();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  serverRunning = true;
  DBG("[HTTP] server on port %u\n", HTTP_PORT);

  xTaskCreatePinnedToCore(liveTask, "live", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "oled", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  server.handleClient();

  const uint32_t now = millis();

  static uint32_t lastClientsMs = 0;
  if (now - lastClientsMs >= 500) {
    lastClientsMs = now;
    apClients = WiFi.softAPgetStationNum();
  }

  static uint32_t lastApMs = 0;
  if (!apRunning && now - lastApMs >= AP_RETRY_MS) {
    lastApMs = now;
    startAccessPoint();
  }

#if DEBUG_SERIAL
  static uint32_t lastDbgMs = 0;
  if (now - lastDbgMs >= DEBUG_INTERVAL_MS) {
    lastDbgMs = now;
    const SensorData d = readSensor();
    DBG("[TX] seq=%lu raw=%u pct=%u sw=%u clients=%u heap=%lu min=%lu\n",
        (unsigned long)d.sequence, d.potRaw, d.potPercent, d.switchOn, apClients,
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  }
#endif
}
