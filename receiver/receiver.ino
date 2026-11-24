/*
 * ESP32_Wireless_Monitoring - RECEIVER (RX)
 *
 * Joins the transmitter hotspot "esp_tx", receives its live UDP stream (backup:
 * polls http://192.168.4.1/api/data), drives three range LEDs, a communication LED and a buzzer driver, and hosts
 * its own dashboard while connected as a Wi-Fi station.
 *
 *   GET /            dashboard (rx_dashboard.h)
 *   GET /api/data    last received TX values + link and output state
 *   GET /api/status  Wi-Fi, addresses, data age, last request result
 *
 * Target : classic ESP32 (ESP32-WROOM-32 / DevKit V1), Arduino-ESP32 core 3.x
 * Built  : core 3.3.12, ArduinoJson 7.4.3, Adafruit GFX 2.0.0,
 *          Adafruit SSD1306 2.5.17, Adafruit BusIO 1.17.4
 *
 * Task layout. The outputs never depend on a network call returning:
 *   outputTask  core 1, prio 5 - decides and drives LEDs/buzzer every 10 ms,
 *                                supervised by the ESP32 task watchdog
 *   netTask     core 0, prio 2 - Wi-Fi state machine + HTTP polling (may block)
 *   loop()      core 1, prio 1 - HTTP server
 *   displayTask core 0, prio 1 - OLED (the only user of the I2C bus)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include "rx_dashboard.h"

// Declared first because the configuration section uses them.
enum class BuzzerMode : uint8_t { OFF, BEEP, ON };
enum class RangeLed   : uint8_t { NONE, GREEN, YELLOW, RED };
enum class CommLed    : uint8_t { OFF, BLINK, ON };
enum class LinkState  : uint8_t { DISCONNECTED, TIMEOUT, LIVE };
enum class WifiState  : uint8_t { DISCONNECTED, CONNECTING, CONNECTED };

// ============================================================================
//  CONFIGURATION
// ============================================================================

// --- Debug ------------------------------------------------------------------
#define DEBUG_SERIAL 1                          // 0 removes all serial output
constexpr uint32_t SERIAL_BAUD        = 115200;
constexpr uint32_t DEBUG_INTERVAL_MS  = 5000;

// --- GPIO (authoritative pin map, see docs/PINOUT.md) -----------------------
constexpr uint8_t PIN_OLED_SDA   = 21;
constexpr uint8_t PIN_OLED_SCL   = 22;
constexpr uint8_t PIN_LED_RED    = 13;          // range 67-100 %
constexpr uint8_t PIN_LED_GREEN  = 14;          // range 0-33 %
constexpr uint8_t PIN_LED_YELLOW = 26;          // range 34-66 %
constexpr uint8_t PIN_LED_COMM   = 25;          // green communication indicator
constexpr uint8_t PIN_BUZZER     = 27;          // transistor/MOSFET driver input

constexpr bool LED_ACTIVE_HIGH    = true;       // false for active-low LED modules
constexpr bool BUZZER_ACTIVE_HIGH = true;       // true for the NPN/N-MOSFET driver

// --- Wi-Fi station ----------------------------------------------------------
constexpr char     WIFI_SSID[]             = "esp_tx";
constexpr char     WIFI_PASSWORD[]         = "esp32@2027";
constexpr uint32_t WIFI_ATTEMPT_TIMEOUT_MS = 10000;  // give up on one attempt
constexpr uint32_t WIFI_RETRY_MIN_MS       = 500;    // first wait, then doubled
constexpr uint32_t WIFI_RETRY_MAX_MS       = 3000;   // up to this
constexpr uint16_t WIFI_BEACON_LOSS_S      = 3;      // detect a lost AP (min 3)
constexpr uint32_t LINK_STUCK_MS           = 15000;  // Wi-Fi up but no data: rejoin

// --- Transmitter API (base URL http://192.168.4.1) --------------------------
constexpr char     TX_HOST[]      = "192.168.4.1";
constexpr uint16_t TX_PORT        = 80;
constexpr char     TX_DATA_PATH[] = "/api/data";
constexpr char     TX_DEVICE_ID[] = "ESP32_TX";
constexpr char     TX_STATUS_OK[] = "ONLINE";

// --- Timing -----------------------------------------------------------------
// Live stream (UDP): the TX pushes every sample. HTTP polling is the backup.
constexpr uint16_t LIVE_PORT         = 4210;    // must match the transmitter
constexpr char     LIVE_SUBSCRIBE[]  = "SUB";   // must match the transmitter
constexpr uint32_t LIVE_RENEW_MS     = 500;     // ask again, well below the TX lease (2000)
constexpr uint32_t LIVE_LOST_MS      = 250;     // no stream for this long: poll by HTTP
constexpr uint32_t POLL_INTERVAL_MS  = 150;     // start to start (target 100-200)
constexpr uint32_t POLL_ERROR_MS     = 250;     // pause after a failed request
constexpr uint32_t HTTP_TIMEOUT_MS   = 400;     // connect, header and body, each
constexpr uint32_t DATA_TIMEOUT_MS   = 1000;    // outputs forced OFF beyond this
constexpr uint32_t OUTPUT_TICK_MS    = 10;

// --- Range limits (inclusive) -----------------------------------------------
constexpr uint8_t GREEN_MAX_PERCENT  = 33;      // 0-33   green
constexpr uint8_t YELLOW_MAX_PERCENT = 66;      // 34-66  yellow, 67-100 red

// --- Buzzer behaviour (active buzzer: DC on = sound) ------------------------
constexpr BuzzerMode BUZZER_IN_GREEN  = BuzzerMode::OFF;
constexpr BuzzerMode BUZZER_IN_YELLOW = BuzzerMode::BEEP;  // OFF disables the beep
constexpr BuzzerMode BUZZER_IN_RED    = BuzzerMode::ON;
constexpr uint32_t   BEEP_ON_MS       = 100;
constexpr uint32_t   BEEP_PERIOD_MS   = 1000;

// --- Communication indicator ------------------------------------------------
constexpr uint32_t COMM_BLINK_MS     = 250;     // half period while connecting

// --- HTTP server ------------------------------------------------------------
constexpr uint16_t HTTP_PORT         = 80;
constexpr uint32_t HTTP_IDLE_BUSY_MS = 40;      // drop a silent socket if others wait
constexpr uint32_t HTTP_IDLE_MAX_MS  = 1000;    // drop a silent socket otherwise

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
#if ARDUINOJSON_VERSION_MAJOR < 7
#error "ArduinoJson 7.x is required (built with 7.4.3)."
#endif

// A macro, not a function: the Arduino builder inserts prototypes before the
// first function, which must come after the types below.
#define PIN_IS_SAFE(p) (!((p) >= 6 && (p) <= 11) && (p) != 12)
static_assert(PIN_IS_SAFE(PIN_OLED_SDA) && PIN_IS_SAFE(PIN_OLED_SCL) &&
              PIN_IS_SAFE(PIN_LED_RED) && PIN_IS_SAFE(PIN_LED_GREEN) &&
              PIN_IS_SAFE(PIN_LED_YELLOW) && PIN_IS_SAFE(PIN_LED_COMM) &&
              PIN_IS_SAFE(PIN_BUZZER),
              "GPIO6-11 (flash) and GPIO12 (boot strap) must not be used");
static_assert(GREEN_MAX_PERCENT < YELLOW_MAX_PERCENT && YELLOW_MAX_PERCENT < 100,
              "range limits must be increasing");

#if DEBUG_SERIAL
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

// ============================================================================
//  TYPES AND SHARED STATE
// ============================================================================

enum class ReqResult : uint8_t {
  NONE,            // nothing requested yet
  OK,
  CONNECT_FAILED,  // TCP connection refused / not reachable
  HTTP_TIMEOUT,    // no complete answer in time
  HTTP_ERROR,      // any other transport problem
  HTTP_STATUS,     // answered, but not 200
  BAD_LENGTH,      // Content-Length missing, zero or too large
  BAD_JSON,        // not a JSON object
  BAD_FIELDS,      // key missing, wrong type or out of range
  TX_FAULT,        // TX reports a status other than ONLINE
  STALE_SEQUENCE   // same sequence as before: TX data is frozen
};

struct TxPacket {
  uint16_t potRaw;
  uint8_t  potPercent;
  uint8_t  switchOn;
  uint16_t wifiClients;
  uint32_t uptimeMs;
  uint32_t sequence;
};

// Written by netTask only.
struct LinkShared {
  TxPacket  packet;
  bool      received;      // at least one valid packet since boot
  bool      trusted;       // false after a malformed payload or Wi-Fi loss
  uint32_t  lastValidMs;
  WifiState wifi;
  uint32_t  wifiJoins;
  ReqResult lastResult;
  int       lastHttpCode;
  uint32_t  lastLatencyMs;
  uint32_t  okCount;
  uint32_t  failCount;
};

// Written by outputTask only.
struct OutputShared {
  LinkState  link;
  RangeLed   led;
  CommLed    comm;
  BuzzerMode buzzer;
};

struct Snapshot {
  LinkShared   link;
  OutputShared out;
  uint32_t     nowMs;
};

static portMUX_TYPE  stateMux = portMUX_INITIALIZER_UNLOCKED;
static LinkShared    linkState = {};
static OutputShared  outState  = {};
static volatile bool wifiDropEvent = false;
static volatile bool oledOk        = false;
static volatile bool serverRunning = false;

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

// The time is taken inside the lock so that age can never be negative.
static Snapshot takeSnapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.link  = linkState;
  s.out   = outState;
  s.nowMs = millis();
  portEXIT_CRITICAL(&stateMux);
  return s;
}

// ============================================================================
//  DECISION LOGIC (pure functions, no hardware access)
// ============================================================================
// BEGIN_PURE_LOGIC

// LIVE is the only state in which received values may drive an output.
static LinkState evalLink(WifiState wifi, bool received, bool trusted, uint32_t ageMs) {
  if (wifi != WifiState::CONNECTED) return LinkState::DISCONNECTED;
  if (!received || !trusted || ageMs > DATA_TIMEOUT_MS) return LinkState::TIMEOUT;
  return LinkState::LIVE;
}

// Follows the potentiometer only. The switch must not affect the range LEDs.
static RangeLed evalRange(LinkState link, uint8_t percent) {
  if (link != LinkState::LIVE) return RangeLed::NONE;
  if (percent <= GREEN_MAX_PERCENT)  return RangeLed::GREEN;
  if (percent <= YELLOW_MAX_PERCENT) return RangeLed::YELLOW;
  return RangeLed::RED;
}

// Follows the range LED. The switch must not affect the buzzer.
static BuzzerMode evalBuzzer(RangeLed led) {
  switch (led) {
    case RangeLed::GREEN:  return BUZZER_IN_GREEN;
    case RangeLed::YELLOW: return BUZZER_IN_YELLOW;
    case RangeLed::RED:    return BUZZER_IN_RED;
    default:               return BuzzerMode::OFF;
  }
}

static bool buzzerLevel(BuzzerMode mode, uint32_t sinceModeMs) {
  if (mode == BuzzerMode::ON)   return true;
  if (mode == BuzzerMode::BEEP) return (sinceModeMs % BEEP_PERIOD_MS) < BEEP_ON_MS;
  return false;
}

// Independent from the range LED: looks only at Wi-Fi and data freshness.
static CommLed evalComm(WifiState wifi, LinkState link) {
  if (wifi == WifiState::CONNECTING) return CommLed::BLINK;
  return link == LinkState::LIVE ? CommLed::ON : CommLed::OFF;
}

// END_PURE_LOGIC

// ============================================================================
//  OUTPUTS
// ============================================================================

static void writeLed(uint8_t pin, bool on)  { digitalWrite(pin, on == LED_ACTIVE_HIGH ? HIGH : LOW); }
static void writeBuzzer(bool on)            { digitalWrite(PIN_BUZZER, on == BUZZER_ACTIVE_HIGH ? HIGH : LOW); }

// Sets the OFF level in the output register before the pin becomes an output,
// so there is no glitch even with active-low hardware.
static void initOutputOff(uint8_t pin, bool activeHigh) {
  gpio_set_level((gpio_num_t)pin, activeHigh ? 0 : 1);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, activeHigh ? LOW : HIGH);
}

static void initOutputs() {
  initOutputOff(PIN_BUZZER,     BUZZER_ACTIVE_HIGH);
  initOutputOff(PIN_LED_RED,    LED_ACTIVE_HIGH);
  initOutputOff(PIN_LED_GREEN,  LED_ACTIVE_HIGH);
  initOutputOff(PIN_LED_YELLOW, LED_ACTIVE_HIGH);
  initOutputOff(PIN_LED_COMM,   LED_ACTIVE_HIGH);
}

static void applyRange(RangeLed led) {
  // Switch off first so that two range LEDs are never on together.
  if (led != RangeLed::GREEN)  writeLed(PIN_LED_GREEN, false);
  if (led != RangeLed::YELLOW) writeLed(PIN_LED_YELLOW, false);
  if (led != RangeLed::RED)    writeLed(PIN_LED_RED, false);
  if (led == RangeLed::GREEN)  writeLed(PIN_LED_GREEN, true);
  if (led == RangeLed::YELLOW) writeLed(PIN_LED_YELLOW, true);
  if (led == RangeLed::RED)    writeLed(PIN_LED_RED, true);
}

static void outputTask(void *) {
  const bool wdt = esp_task_wdt_add(nullptr) == ESP_OK;
  BuzzerMode lastMode = BuzzerMode::OFF;
  uint32_t modeSinceMs = 0;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const Snapshot s = takeSnapshot();

    OutputShared o;
    o.link   = evalLink(s.link.wifi, s.link.received, s.link.trusted, s.nowMs - s.link.lastValidMs);
    o.led    = evalRange(o.link, s.link.packet.potPercent);
    o.buzzer = evalBuzzer(o.led);
    o.comm   = evalComm(s.link.wifi, o.link);

    if (o.buzzer != lastMode) {
      lastMode = o.buzzer;
      modeSinceMs = s.nowMs;
    }

    writeBuzzer(buzzerLevel(o.buzzer, s.nowMs - modeSinceMs));
    applyRange(o.led);
    writeLed(PIN_LED_COMM, o.comm == CommLed::ON ||
             (o.comm == CommLed::BLINK && ((s.nowMs / COMM_BLINK_MS) & 1)));

    portENTER_CRITICAL(&stateMux);
    outState = o;
    portEXIT_CRITICAL(&stateMux);

    if (wdt) esp_task_wdt_reset();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(OUTPUT_TICK_MS));
  }
}

// ============================================================================
//  WI-FI STATE MACHINE (netTask only)
// ============================================================================

static WifiState netState = WifiState::DISCONNECTED;
static uint32_t  netStateMs = 0;
static uint32_t  retryWaitMs = 0;               // 0: first attempt immediately
static uint32_t  lastDataOkMs = 0;
static WiFiUDP   liveUdp;
static IPAddress txIp;                          // TX_HOST, set in setup()
static uint32_t  lastLiveMs = 0;                // last valid stream packet
static uint32_t  lastSubscribeMs = 0;

static void setWifiState(WifiState st) {
  netState   = st;
  netStateMs = millis();
  portENTER_CRITICAL(&stateMux);
  linkState.wifi = st;
  if (st != WifiState::CONNECTED) linkState.trusted = false;  // outputs OFF at once
  if (st == WifiState::CONNECTED) linkState.wifiJoins++;
  portEXIT_CRITICAL(&stateMux);
}

static void scheduleRetry() {
  retryWaitMs = retryWaitMs == 0 ? WIFI_RETRY_MIN_MS : min(retryWaitMs * 2, WIFI_RETRY_MAX_MS);
  setWifiState(WifiState::DISCONNECTED);
  DBG("[WIFI] down, retry in %lu ms\n", (unsigned long)retryWaitMs);
}

static void onStaConnected() {
  wifiDropEvent = false;
  retryWaitMs   = 0;
  lastDataOkMs  = millis();
  lastLiveMs    = millis();                     // give the stream time to start
  lastSubscribeMs = millis() - LIVE_RENEW_MS;   // ask for it at once
  liveUdp.begin(LIVE_PORT);
  setWifiState(WifiState::CONNECTED);
  DBG("[WIFI] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void serviceWifi() {
  const uint32_t now = millis();
  const bool up = WiFi.status() == WL_CONNECTED;

  switch (netState) {
    case WifiState::DISCONNECTED:
      if (up) {
        onStaConnected();
      } else if (now - netStateMs >= retryWaitMs) {
        DBG("[WIFI] joining %s\n", WIFI_SSID);
        setWifiState(WifiState::CONNECTING);
        WiFi.disconnect(false, false);          // cancel any half-open attempt
        vTaskDelay(pdMS_TO_TICKS(100));         // let its event arrive first
        wifiDropEvent = false;
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        WiFi.STA.setInactiveTime(WIFI_BEACON_LOSS_S);
      }
      break;

    case WifiState::CONNECTING:
      if (up) {
        onStaConnected();
      } else if (wifiDropEvent || now - netStateMs >= WIFI_ATTEMPT_TIMEOUT_MS) {
        scheduleRetry();
      }
      break;

    case WifiState::CONNECTED:
      if (!up || wifiDropEvent) {
        scheduleRetry();
      } else if (now - lastDataOkMs >= LINK_STUCK_MS) {
        DBG("[WIFI] no valid data for %lu ms, rejoining\n", (unsigned long)LINK_STUCK_MS);
        WiFi.disconnect(false, false);
        scheduleRetry();
      }
      break;
  }
}

static void onWifiStaDisconnected(arduino_event_id_t, arduino_event_info_t info) {
  wifiDropEvent = true;
  DBG("[WIFI] disconnect event, reason %u\n", info.wifi_sta_disconnected.reason);
}

// ============================================================================
//  HTTP POLLING (netTask only)
// ============================================================================

constexpr size_t BODY_MAX = 512;

static bool isUInt(JsonVariantConst v, uint32_t maxValue) {
  return v.is<uint32_t>() && v.as<uint32_t>() <= maxValue;
}

// Accepts the packet only if every required key exists with the right type.
static ReqResult parsePacket(const char *body, size_t len, TxPacket &out) {
  static JsonDocument doc;                      // reused, netTask only
  if (deserializeJson(doc, body, len) || !doc.is<JsonObjectConst>()) return ReqResult::BAD_JSON;

  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (!o["device"].is<const char *>() || strcmp(o["device"].as<const char *>(), TX_DEVICE_ID) != 0 ||
      !o["status"].is<const char *>() ||
      !isUInt(o["pot_raw"], 4095) || !isUInt(o["pot_percent"], 100) ||
      !isUInt(o["switch"], 1)     || !isUInt(o["wifi_clients"], 255) ||
      !isUInt(o["uptime_ms"], UINT32_MAX) || !isUInt(o["sequence"], UINT32_MAX)) {
    return ReqResult::BAD_FIELDS;
  }
  if (strcmp(o["status"].as<const char *>(), TX_STATUS_OK) != 0) return ReqResult::TX_FAULT;

  out.potRaw      = o["pot_raw"].as<uint16_t>();
  out.potPercent  = o["pot_percent"].as<uint8_t>();
  out.switchOn    = o["switch"].as<uint8_t>();
  out.wifiClients = o["wifi_clients"].as<uint16_t>();
  out.uptimeMs    = o["uptime_ms"].as<uint32_t>();
  out.sequence    = o["sequence"].as<uint32_t>();
  return ReqResult::OK;
}

// Reads exactly `len` body bytes within HTTP_TIMEOUT_MS overall.
static bool readBody(NetworkClient &client, char *buf, size_t len) {
  const uint32_t start = millis();
  size_t got = 0;
  while (got < len) {
    if (millis() - start >= HTTP_TIMEOUT_MS) return false;
    const int avail = client.available();
    if (avail > 0) {
      const int n = client.read((uint8_t *)buf + got, min((size_t)avail, len - got));
      if (n > 0) got += n;
    } else if (!client.connected()) {
      return false;
    } else {
      vTaskDelay(1);
    }
  }
  return true;
}

static ReqResult fetchPacket(TxPacket &out, int &httpCode) {
  static NetworkClient client;
  static HTTPClient http;
  static char body[BODY_MAX + 1];

  http.setReuse(false);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, TX_HOST, TX_PORT, TX_DATA_PATH)) {
    httpCode = 0;
    return ReqResult::HTTP_ERROR;
  }

  ReqResult result;
  httpCode = http.GET();
  if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED) {
    result = ReqResult::CONNECT_FAILED;
  } else if (httpCode == HTTPC_ERROR_READ_TIMEOUT) {
    result = ReqResult::HTTP_TIMEOUT;
  } else if (httpCode <= 0) {
    result = ReqResult::HTTP_ERROR;
  } else if (httpCode != HTTP_CODE_OK) {
    result = ReqResult::HTTP_STATUS;
  } else {
    const int len = http.getSize();
    if (len <= 0 || len > (int)BODY_MAX) {
      result = ReqResult::BAD_LENGTH;
    } else if (!readBody(http.getStream(), body, (size_t)len)) {
      result = ReqResult::HTTP_TIMEOUT;
    } else {
      body[len] = '\0';
      result = parsePacket(body, (size_t)len, out);
    }
  }
  http.end();                                   // always releases the socket
  client.stop();
  return result;
}

// A malformed payload means the source cannot be trusted: outputs go OFF at
// once. A lost request only lets the data age until DATA_TIMEOUT_MS.
static bool isPayloadFault(ReqResult r) {
  return r == ReqResult::BAD_LENGTH || r == ReqResult::BAD_JSON ||
         r == ReqResult::BAD_FIELDS || r == ReqResult::TX_FAULT;
}

// Accepts or rejects one packet, whichever way it arrived.
static bool applyPacket(ReqResult r, const TxPacket &p, int code, uint32_t latency) {
  portENTER_CRITICAL(&stateMux);
  if (r == ReqResult::OK && linkState.received && p.sequence == linkState.packet.sequence) {
    r = ReqResult::STALE_SEQUENCE;              // valid syntax, frozen data
  }
  if (r == ReqResult::OK) {
    linkState.packet      = p;
    linkState.received    = true;
    linkState.trusted     = true;
    linkState.lastValidMs = millis();
    linkState.okCount++;
  } else {
    if (isPayloadFault(r)) linkState.trusted = false;
    linkState.failCount++;
  }
  const bool changed      = r != linkState.lastResult;
  linkState.lastResult    = r;
  linkState.lastHttpCode  = code;
  linkState.lastLatencyMs = latency;
  portEXIT_CRITICAL(&stateMux);

  if (r == ReqResult::OK) lastDataOkMs = millis();
  if (changed) DBG("[DATA] %s code=%d %lu ms\n", resultName(r), code, (unsigned long)latency);
  return r == ReqResult::OK;
}

static bool pollTransmitter() {
  TxPacket p = {};
  int code = 0;
  const uint32_t start = millis();
  const ReqResult r = fetchPacket(p, code);
  return applyPacket(r, p, code, millis() - start);
}

// Keeps the stream alive and applies the newest packet that has arrived.
// For stream packets the code is 0 and the latency is the time since the last one.
static void serviceLive() {
  static char body[BODY_MAX + 1];

  if (millis() - lastSubscribeMs >= LIVE_RENEW_MS) {
    lastSubscribeMs = millis();
    if (liveUdp.beginPacket(txIp, LIVE_PORT)) {
      liveUdp.print(LIVE_SUBSCRIBE);
      liveUdp.endPacket();
    }
  }

  int len = 0;
  while (liveUdp.parsePacket() > 0) {           // older packets are skipped
    const int n = liveUdp.read(body, BODY_MAX);
    const bool fromTx = liveUdp.remoteIP() == txIp;
    liveUdp.clear();
    if (fromTx && n > 0) len = n;
    else len = 0;
  }
  if (len == 0) return;

  body[len] = '\0';
  TxPacket p = {};
  const ReqResult r = parsePacket(body, (size_t)len, p);
  if (applyPacket(r, p, 0, millis() - lastLiveMs)) lastLiveMs = millis();
}

static void netTask(void *) {
  uint32_t nextPollMs = 0;
  for (;;) {
    serviceWifi();
    if (netState == WifiState::CONNECTED) serviceLive();
    const uint32_t now = millis();
    if (netState != WifiState::CONNECTED) {
      nextPollMs = now;
    } else if (now - lastLiveMs > LIVE_LOST_MS && (int32_t)(now - nextPollMs) >= 0) {
      const bool ok = pollTransmitter();
      nextPollMs = ok ? now + POLL_INTERVAL_MS : millis() + POLL_ERROR_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ============================================================================
//  NAMES USED IN JSON, ON THE OLED AND ON SERIAL
// ============================================================================

static const char *wifiName(WifiState s) {
  return s == WifiState::CONNECTED ? "CONNECTED" : s == WifiState::CONNECTING ? "CONNECTING" : "DISCONNECTED";
}
static const char *linkName(LinkState s) {
  return s == LinkState::LIVE ? "LIVE" : s == LinkState::TIMEOUT ? "TIMEOUT" : "DISCONNECTED";
}
static const char *ledName(RangeLed l) {
  return l == RangeLed::GREEN ? "GREEN" : l == RangeLed::YELLOW ? "YELLOW" : l == RangeLed::RED ? "RED" : "NONE";
}
static const char *commName(CommLed c) {
  return c == CommLed::ON ? "ON" : c == CommLed::BLINK ? "BLINK" : "OFF";
}
static const char *buzzerName(BuzzerMode b) {
  return b == BuzzerMode::ON ? "ON" : b == BuzzerMode::BEEP ? "BEEP" : "OFF";
}
static const char *resultName(ReqResult r) {
  switch (r) {
    case ReqResult::OK:             return "OK";
    case ReqResult::CONNECT_FAILED: return "CONNECT_FAILED";
    case ReqResult::HTTP_TIMEOUT:   return "HTTP_TIMEOUT";
    case ReqResult::HTTP_ERROR:     return "HTTP_ERROR";
    case ReqResult::HTTP_STATUS:    return "HTTP_STATUS";
    case ReqResult::BAD_LENGTH:     return "BAD_LENGTH";
    case ReqResult::BAD_JSON:       return "BAD_JSON";
    case ReqResult::BAD_FIELDS:     return "BAD_FIELDS";
    case ReqResult::TX_FAULT:       return "TX_FAULT";
    case ReqResult::STALE_SEQUENCE: return "STALE_SEQUENCE";
    default:                        return "NONE";
  }
}

// ============================================================================
//  OLED
// ============================================================================

constexpr uint8_t OLED_ROWS = 8;
constexpr uint8_t OLED_COLS = 21;
static char oledNew[OLED_ROWS][OLED_COLS + 1];
static char oledOld[OLED_ROWS][OLED_COLS + 1];

static IPAddress rxIp(const Snapshot &s) {
  return s.link.wifi == WifiState::CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
}

static bool oledPresent() {
  Wire.beginTransmission(OLED_ADDRESS);
  return Wire.endTransmission() == 0;
}

static void oledCompose() {
  const Snapshot s = takeSnapshot();
  const IPAddress ip = rxIp(s);
  const bool connected = s.link.wifi == WifiState::CONNECTED;
  snprintf(oledNew[0], sizeof(oledNew[0]), "ESP32 RECEIVER");
  snprintf(oledNew[1], sizeof(oledNew[1]), "WiFi:%s", wifiName(s.link.wifi));
  snprintf(oledNew[2], sizeof(oledNew[2]), "SSID:%s", connected ? WIFI_SSID : "--");
  if (connected) snprintf(oledNew[3], sizeof(oledNew[3]), "RX:%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  else           snprintf(oledNew[3], sizeof(oledNew[3]), "RX:--");
  snprintf(oledNew[4], sizeof(oledNew[4]), "TX:%s", TX_HOST);
  snprintf(oledNew[5], sizeof(oledNew[5]), "Link:%s", linkName(s.out.link));
  if (s.link.received) {
    // "old" marks the last values once they are no longer live.
    snprintf(oledNew[6], sizeof(oledNew[6]), "Pot:%u%% SW:%s%s", s.link.packet.potPercent,
             s.link.packet.switchOn ? "ON" : "OFF", s.out.link == LinkState::LIVE ? "" : " old");
  } else {
    snprintf(oledNew[6], sizeof(oledNew[6]), "Pot:-- SW:--");
  }
  snprintf(oledNew[7], sizeof(oledNew[7]), "LED:%s BUZ:%s", ledName(s.out.led), buzzerName(s.out.buzzer));
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
//  HTTP SERVER (loop only)
// ============================================================================

static char jsonBuf[768];

static void sendJson(int len) {
  if (len < 0 || len >= (int)sizeof(jsonBuf)) {
    server.send(500, "application/json", "{\"error\":\"BUFFER\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "application/json", jsonBuf, (size_t)len);
}

// Age of the last valid packet, -1 if none was ever received.
static long dataAgeMs(const Snapshot &s) {
  return s.link.received ? (long)min(s.nowMs - s.link.lastValidMs, (uint32_t)INT32_MAX) : -1L;
}

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", RX_DASHBOARD_HTML, RX_DASHBOARD_HTML_LEN);
}

static void handleData() {
  const Snapshot s = takeSnapshot();
  const TxPacket &p = s.link.packet;
  sendJson(snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"device\":\"ESP32_RX\",\"pot_raw\":%u,\"pot_percent\":%u,\"switch\":%u,"
    "\"wifi_clients\":%u,\"uptime_ms\":%lu,\"tx_uptime_ms\":%lu,\"sequence\":%lu,"
    "\"status\":\"ONLINE\",\"link_state\":\"%s\",\"data_received\":%u,\"data_fresh\":%u,"
    "\"data_age_ms\":%ld,\"active_led\":\"%s\",\"comm_led\":\"%s\",\"buzzer\":\"%s\","
    "\"last_result\":\"%s\",\"last_http_code\":%d,\"last_latency_ms\":%lu}",
    p.potRaw, p.potPercent, p.switchOn, p.wifiClients,
    (unsigned long)s.nowMs, (unsigned long)p.uptimeMs, (unsigned long)p.sequence,
    linkName(s.out.link), s.link.received ? 1 : 0, s.out.link == LinkState::LIVE ? 1 : 0,
    dataAgeMs(s), ledName(s.out.led), commName(s.out.comm), buzzerName(s.out.buzzer),
    resultName(s.link.lastResult), s.link.lastHttpCode, (unsigned long)s.link.lastLatencyMs));
}

static void handleStatus() {
  const Snapshot s = takeSnapshot();
  const IPAddress ip = rxIp(s);
  const int rssi = s.link.wifi == WifiState::CONNECTED ? WiFi.RSSI() : 0;
  sendJson(snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"device\":\"ESP32_RX\",\"status\":\"ONLINE\",\"wifi_state\":\"%s\",\"ssid\":\"%s\","
    "\"rx_ip\":\"%u.%u.%u.%u\",\"tx_ip\":\"%s\",\"rssi_dbm\":%d,\"link_state\":\"%s\","
    "\"data_age_ms\":%ld,\"last_result\":\"%s\",\"last_http_code\":%d,\"last_latency_ms\":%lu,"
    "\"packets_ok\":%lu,\"packets_failed\":%lu,\"wifi_joins\":%lu,\"uptime_ms\":%lu,"
    "\"server_state\":\"%s\",\"oled_state\":\"%s\",\"free_heap\":%lu}",
    wifiName(s.link.wifi), WIFI_SSID, ip[0], ip[1], ip[2], ip[3], TX_HOST, rssi,
    linkName(s.out.link), dataAgeMs(s), resultName(s.link.lastResult), s.link.lastHttpCode,
    (unsigned long)s.link.lastLatencyMs, (unsigned long)s.link.okCount,
    (unsigned long)s.link.failCount, (unsigned long)s.link.wifiJoins, (unsigned long)s.nowMs,
    serverRunning ? "RUNNING" : "STOPPED", oledOk ? "OK" : "NOT_FOUND",
    (unsigned long)ESP.getFreeHeap()));
}

static void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"NOT_FOUND\"}");
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

void setup() {
  initOutputs();                                // before anything else

#if DEBUG_SERIAL
  Serial.begin(SERIAL_BAUD);
#endif
  DBG("\n[RX] boot\n");

  xTaskCreatePinnedToCore(outputTask, "output", 3072, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(displayTask, "oled", 4096, nullptr, 1, nullptr, 0);

  txIp.fromString(TX_HOST);
  WiFi.persistent(false);
  WiFi.onEvent(onWifiStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                         // lowest latency
  WiFi.setAutoReconnect(false);                 // serviceWifi() owns reconnecting

  // Listens on all interfaces, so it works as soon as DHCP assigns an address.
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  serverRunning = true;
  DBG("[HTTP] server on port %u\n", HTTP_PORT);

  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 2, nullptr, 0);
}

void loop() {
  server.handleClient();

#if DEBUG_SERIAL
  static uint32_t lastDbgMs = 0;
  if (millis() - lastDbgMs >= DEBUG_INTERVAL_MS) {
    lastDbgMs = millis();
    const Snapshot s = takeSnapshot();
    DBG("[RX] wifi=%s link=%s age=%ld pct=%u sw=%u led=%s buz=%s res=%s lat=%lu ok=%lu fail=%lu heap=%lu min=%lu\n",
        wifiName(s.link.wifi), linkName(s.out.link), dataAgeMs(s),
        s.link.packet.potPercent, s.link.packet.switchOn, ledName(s.out.led),
        buzzerName(s.out.buzzer), resultName(s.link.lastResult),
        (unsigned long)s.link.lastLatencyMs, (unsigned long)s.link.okCount, (unsigned long)s.link.failCount,
        (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  }
#endif
}
