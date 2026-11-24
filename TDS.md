# Technical Design Specification

**Project:** ESP32 Wireless Monitoring and Control System
**Revision:** 1.0 (as implemented)
**Date:** 24 November 2026
**Network:** local Wi-Fi only, no internet, router or cloud

Details live in one place each: pins in [PINOUT.md](PINOUT.md), parts and
electrical notes in [HARDWARE.md](HARDWARE.md), routes, JSON and timing in
[COMMUNICATION.md](COMMUNICATION.md).

## 1. System

```text
     TRANSMITTER (ESP32)                         RECEIVER (ESP32)
+---------------------------+            +-------------------------------+
| Potentiometer  -> GPIO34  |            | Red LED (67-100 %)  -> GPIO13 |
| Push button    -> GPIO27  |   Wi-Fi    | Green LED (0-33 %)  -> GPIO14 |
| OLED SDA/SCL   -> 21 / 22 |   esp_tx   | Yellow LED (34-66 %)-> GPIO26 |
|                           |<-----------| Green comm LED      -> GPIO25 |
| Access point 192.168.4.1  | GET        | Buzzer driver       -> GPIO27 |
| HTTP API + TX dashboard   | /api/data  | OLED SDA/SCL        -> 21 / 22|
+---------------------------+            | Station (DHCP), RX dashboard  |
                                         +-------------------------------+
```

The TX is the data source and network owner. The RX pulls data, decides the
outputs, and hosts its own HTTP server while joined as a station.

## 2. Firmware structure

| Sketch | Task | Core / priority | Work | May block on |
|---|---|---|---|---|
| TX | `sampleTask` | 1 / 3 | Button every 5 ms, ADC every 20 ms | Nothing |
| TX | `loop()` | 1 / 1 | HTTP server, client count, AP retry | Network |
| TX | `displayTask` | 0 / 1 | OLED | I2C |
| RX | `outputTask` | 1 / 5 | Decide and drive outputs every 10 ms | Nothing |
| RX | `netTask` | 0 / 2 | Wi-Fi state machine, HTTP polling | Network |
| RX | `loop()` | 1 / 1 | HTTP server | Network |
| RX | `displayTask` | 0 / 1 | OLED | I2C |

Tasks exchange data through small structures copied under a spinlock. Only one
task uses the I2C bus. Because `outputTask` has the highest priority and never
waits for the network, a hanging request cannot delay the safe state. It is
registered with the ESP32 task watchdog: if it stops, the chip resets and boots
with outputs OFF.

Memory: dashboards stay in flash, JSON is written with `snprintf` into static
buffers, request bodies go into a fixed 512-byte buffer, and the ArduinoJson
document is reused. The Arduino `WebServer` and `HTTPClient` still make small
internal `String` allocations per request. Free and minimum heap are printed
every 5 s for monitoring.

ArduinoJson is used only by the RX, to parse and validate.

## 3. Sensor processing (TX)

1. GPIO34, 12 bit, 11 dB attenuation, average of 8 readings.
2. Exponential smoothing with weight 1/2 per 20 ms sample.
3. Deadband of 8 counts against flicker at range borders.
4. `percent = round((raw - POT_RAW_MIN) * 100 / (POT_RAW_MAX - POT_RAW_MIN))`,
   clamped to 0-100.
5. Button: `INPUT_PULLUP`, stable for 30 ms, LOW = `switch` 1. It is momentary,
   and currently controls no output.

The ESP32 ADC is not linear or precisely calibrated. The percentage is a control
value, not a measurement.

## 4. Output logic (RX)

Outputs may be ON only if `link_state` is `LIVE`. The switch has no effect.

| Percent | Range LED | Buzzer | Setting |
|---|---|---|---|
| 0-33 | Green GPIO14 | OFF | `BUZZER_IN_GREEN` |
| 34-66 | Yellow GPIO26 | Beep, 100 ms every 1000 ms | `BUZZER_IN_YELLOW`, `BEEP_ON_MS`, `BEEP_PERIOD_MS` |
| 67-100 | Red GPIO13 | Continuous | `BUZZER_IN_RED` |

Only one range LED is ON at a time: the others are switched off first.

Communication LED (GPIO25), independent of the range LEDs:

| Condition | LED |
|---|---|
| Join attempt running | Blink |
| Wi-Fi connected and data `LIVE` | Steady ON |
| `TIMEOUT`, or Wi-Fi down between attempts | OFF |

## 5. Failure behaviour

| Failure | Reaction |
|---|---|
| RX reset | Output registers set to OFF before the pins become outputs, first lines of `setup()` |
| TX without power | Outputs OFF within 1 s (data age), then `DISCONNECTED` |
| Wi-Fi lost | Outputs OFF at once, retries after 0.5 / 1 / 2 / 3 s |
| Request fails | Last value kept until it is 1000 ms old, then OFF |
| Malformed payload or TX `FAULT` | Packet rejected, timestamp not refreshed, outputs OFF at once |
| TX answers but data frozen | Same `sequence` rejected, outputs OFF after 1000 ms |
| Wi-Fi up, no valid data for 15 s | RX rejoins the network |
| Switch OFF | No effect on range LEDs or buzzer |
| Output task stops | Watchdog reset, outputs OFF |
| OLED missing or unplugged | Everything else continues, retry every 5 s |
| Access point fails to start | Retry every 5 s |

## 6. OLED

Eight lines of 21 characters. Content is compared every 200 ms and sent only if
it changed. The RX marks values that are no longer live with `old`.

| Line | TX | RX |
|---|---|---|
| 1 | ESP32 TRANSMITTER | ESP32 RECEIVER |
| 2 | AP state, clients | Wi-Fi state |
| 3 | SSID | SSID (`--` if not connected) |
| 4 | IP | RX IP |
| 5 | Pot raw | TX IP |
| 6 | Pot percent | LIVE / TIMEOUT / DISCONNECTED |
| 7 | Button | Pot percent, switch |
| 8 | HTTP server, device status | Active LED, buzzer |

## 7. Dashboards

Plain HTML, CSS and JavaScript in one flash string per header file, no external
resources. One request at a time: data every 400 ms, status every 2 s in place
of a data request. Polling pauses in hidden tabs and slows after errors. States:
loading, live/online, timeout and disconnected (RX), API error, offline. Stale
values are dimmed and labelled.

## 8. Decisions where the specification was open

| Topic | Decision |
|---|---|
| GPIO25 while reconnecting vs. disconnected | Blinks during a join attempt, OFF during the pause |
| Invalid data | Malformed payload: OFF at once. Lost request: OFF after 1000 ms |
| Beep settings | 100 ms every 1000 ms |
| Wi-Fi channel, station limit | 6, 6 |
| Buzzer type | Active buzzer, NPN low-side driver, rating open |
| LED resistors | 220 Ω |
| Extra packet check | `sequence` must change |
| Chip | Classic ESP32 only, enforced at compile time |

## 9. Open hardware items

1. Board exposes GPIO25 and GPIO26 and is an ESP32-WROOM-32 type.
2. OLED controller, address, supply range and pin order.
3. LED polarity and forward voltage.
4. Buzzer voltage, current and type; Q1, R5, D5 selected from them.
5. Timing targets under real radio conditions.

Acceptance tests are listed in [INSTALLATION.md](INSTALLATION.md).
