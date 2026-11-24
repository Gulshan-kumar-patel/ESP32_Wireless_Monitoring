# Communication

## Wi-Fi

| Parameter | TX | RX |
|---|---|---|
| Mode | Access point | Station |
| SSID | `esp_tx` | `esp_tx` |
| Password | `esp32@2027` (WPA2) | `esp32@2027` |
| IP | `192.168.4.1` (fixed) | DHCP, shown on the OLED and dashboard |
| Channel | 6 (`AP_CHANNEL`) | Found by scan |
| HTTP port | 80 | 80 |

The RX uses HTTP client (to TX) and HTTP server (for its dashboard) at the same
time on the same station interface. A phone or laptop joins `esp_tx` and opens
`http://192.168.4.1/` or `http://<receiver-ip>/`. Up to 6 stations may join,
including the RX (`AP_MAX_CLIENTS`).

### RX connection state machine

| `wifi_state` | Meaning | Communication LED (GPIO25) |
|---|---|---|
| `CONNECTING` | A join attempt is running (limit 10 s) | Blink, 250 ms on / 250 ms off |
| `DISCONNECTED` | Pause before the next attempt: 0.5, 1, 2, then 3 s | OFF |
| `CONNECTED` | Joined and has an IP | ON if data is fresh, otherwise OFF |

So while the TX is off the LED alternates between blinking (trying) and OFF
(waiting). The core's own auto-reconnect is disabled so that exactly one piece
of code retries. A lost access point is detected after about 3 s of missing
beacons (`WIFI_BEACON_LOSS_S`). If Wi-Fi stays up but no valid packet arrives
for 15 s (`LINK_STUCK_MS`), the RX leaves and rejoins.

## Polling and timing

| Constant | Sketch | Default | Purpose and tuning |
|---|---|---|---|
| `SAMPLE_INTERVAL_MS` | TX | 20 | ADC and sequence rate. Keep 20-50 |
| `BUTTON_DEBOUNCE_MS` | TX | 30 | Raise for a bouncy button |
| `ADC_SMOOTH_SHIFT` | TX | 1 | 0 = no filter; higher = calmer, slower |
| `POT_DEADBAND_RAW` | TX | 8 | Raise if the percentage flickers |
| `POT_RAW_MIN`, `POT_RAW_MAX` | TX | 0, 4095 | Narrow if the ends do not reach 0/100 % |
| `LIVE_PORT` | both | 4210 | UDP stream. RX sends `SUB` every `LIVE_RENEW_MS` (500), TX then sends the `/api/data` JSON for every sample |
| `LIVE_LEASE_MS` | TX | 2000 | Stream stops if RX stops asking |
| `LIVE_LOST_MS` | RX | 250 | No stream for this long: RX polls by HTTP |
| `POLL_INTERVAL_MS` | RX | 150 | Backup polling. Keep 100-200 |
| `POLL_ERROR_MS` | RX | 250 | Pause after a failed request |
| `HTTP_TIMEOUT_MS` | RX | 400 | Limit for connect, header and body, each |
| `DATA_TIMEOUT_MS` | RX | 1000 | Outputs OFF beyond this age. Keep well above `POLL_INTERVAL_MS + HTTP_TIMEOUT_MS` so that one lost request does not trip it |
| `HTTP_IDLE_BUSY_MS` | both | 40 | How long a silent socket may hold the server while others wait |
| `HTTP_IDLE_MAX_MS` | both | 1000 | Same, when nobody waits |
| Dashboard refresh | headers | 400 | `DATA_MS` in the script; status every 2000 |

These are starting values, not measured performance.

### Why the idle limits exist

Arduino's `WebServer` serves one connection at a time. If a browser opens a
socket in advance and sends nothing, the stock server waits 5 s for it. That is
longer than the 1 s data timeout, so one phone could switch the receiver outputs
off. `ResponsiveWebServer` (in both sketches) drops such sockets early; browsers
simply reconnect.

## Packet acceptance (RX)

Checked in this order. Only a packet that passes everything updates the stored
values and the last-valid timestamp.

| Check | `last_result` on failure | Effect on outputs |
|---|---|---|
| TCP connection | `CONNECT_FAILED` | Keep until `DATA_TIMEOUT_MS` |
| Complete answer in time | `HTTP_TIMEOUT` | Keep until `DATA_TIMEOUT_MS` |
| Other transport errors | `HTTP_ERROR` | Keep until `DATA_TIMEOUT_MS` |
| HTTP status is 200 | `HTTP_STATUS` | Keep until `DATA_TIMEOUT_MS` |
| Content-Length 1-512 | `BAD_LENGTH` | **OFF at once** |
| Body is a JSON object | `BAD_JSON` | **OFF at once** |
| All keys present, right type, in range, `device` is `ESP32_TX` | `BAD_FIELDS` | **OFF at once** |
| `status` is `ONLINE` | `TX_FAULT` | **OFF at once** |
| `sequence` differs from the last packet | `STALE_SEQUENCE` | Keep until `DATA_TIMEOUT_MS` |
| All passed | `OK` | Values applied |

`NONE` means no request has been made yet. "OFF at once" lasts until the next
valid packet. Wi-Fi loss also switches outputs OFF at once.

## Link state

| `link_state` | Condition |
|---|---|
| `DISCONNECTED` | Wi-Fi is not connected |
| `TIMEOUT` | Wi-Fi connected, but no valid packet yet, last one older than 1000 ms, or last payload malformed |
| `LIVE` | Wi-Fi connected and fresh valid data. Only now may outputs switch on |

## HTTP routes

Only GET is served. Anything else returns `404 {"error":"NOT_FOUND"}`.
JSON answers carry `Cache-Control: no-store`.

| Device | Path | Content |
|---|---|---|
| TX | `/` | TX dashboard |
| TX | `/api/data` | Sensor data |
| TX | `/api/status` | Access point and server status |
| RX | `/` | RX dashboard |
| RX | `/api/data` | Last received values plus link and output state |
| RX | `/api/status` | Wi-Fi, addresses, data age, last request |

### TX `/api/data`

```json
{"device":"ESP32_TX","pot_raw":2048,"pot_percent":50,"switch":1,
 "wifi_clients":1,"uptime_ms":10000,"sequence":125,"status":"ONLINE"}
```

| Key | Type | Meaning |
|---|---|---|
| `device` | string | Always `ESP32_TX` |
| `pot_raw` | integer 0-4095 | Filtered ADC value |
| `pot_percent` | integer 0-100 | Scaled value |
| `switch` | integer | 1 = pressed/ON, 0 = released/OFF |
| `wifi_clients` | integer | Stations joined to the hotspot |
| `uptime_ms` | unsigned 32-bit | TX uptime, wraps after 49.7 days |
| `sequence` | unsigned 32-bit | +1 per sample (every 20 ms) |
| `status` | string | `ONLINE`, or `FAULT` if sampling stopped for 500 ms |

### TX `/api/status`

```json
{"device":"ESP32_TX","status":"ONLINE","ap_state":"ON","ssid":"esp_tx",
 "ip":"192.168.4.1","wifi_clients":1,"uptime_ms":10000,"sequence":125,
 "server_state":"RUNNING","oled_state":"OK","free_heap":210000}
```

`ap_state`: `ON`/`OFF`. `server_state`: `RUNNING`/`STOPPED`.
`oled_state`: `OK`/`NOT_FOUND`. `free_heap`: bytes.

### RX `/api/data`

```json
{"device":"ESP32_RX","pot_raw":2048,"pot_percent":50,"switch":1,
 "wifi_clients":2,"uptime_ms":65000,"tx_uptime_ms":10000,"sequence":125,
 "status":"ONLINE","link_state":"LIVE","data_received":1,"data_fresh":1,
 "data_age_ms":87,"active_led":"YELLOW","comm_led":"ON","buzzer":"BEEP",
 "last_result":"OK","last_http_code":200,"last_latency_ms":14}
```

| Key | Meaning |
|---|---|
| `pot_raw`, `pot_percent`, `switch`, `wifi_clients`, `sequence` | Last accepted TX values, 0 before the first packet |
| `uptime_ms`, `tx_uptime_ms` | RX uptime; TX uptime from the last packet |
| `status` | RX device state, always `ONLINE` |
| `link_state` | `LIVE`, `TIMEOUT`, `DISCONNECTED` |
| `data_received` | 1 if any valid packet arrived since boot |
| `data_fresh` | 1 only when `link_state` is `LIVE`; otherwise values are stale |
| `data_age_ms` | Age of the last valid packet, -1 if none |
| `active_led` | `NONE`, `GREEN`, `YELLOW`, `RED` |
| `comm_led` | `OFF`, `BLINK`, `ON` |
| `buzzer` | `OFF`, `BEEP`, `ON` |
| `last_result` | See the packet acceptance table |
| `last_http_code` | HTTP code, or a negative HTTPClient error, or 0 (always 0 for stream packets) |
| `last_latency_ms` | Time since the previous stream packet, or duration of the last HTTP request |

### RX `/api/status`

```json
{"device":"ESP32_RX","status":"ONLINE","wifi_state":"CONNECTED","ssid":"esp_tx",
 "rx_ip":"192.168.4.2","tx_ip":"192.168.4.1","rssi_dbm":-48,"link_state":"LIVE",
 "data_age_ms":87,"last_result":"OK","last_http_code":200,"last_latency_ms":14,
 "packets_ok":412,"packets_failed":3,"wifi_joins":1,"uptime_ms":65000,
 "server_state":"RUNNING","oled_state":"OK","free_heap":200000}
```

`rx_ip` is `0.0.0.0` and `rssi_dbm` is 0 while not connected. `packets_failed`
counts every request that was not accepted. `wifi_joins` counts successful joins.

## Security

Traffic is plain HTTP without login; only the Wi-Fi password protects it. All
routes are read-only. Change the password in both sketches for real use.
