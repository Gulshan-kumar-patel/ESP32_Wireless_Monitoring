# Troubleshooting

Start with the Serial Monitor at 115200 baud. Both sketches print one status
line every 5 s and every state change.

## Compile

| Symptom | Cause and fix |
|---|---|
| `ArduinoJson.h: No such file` | Install ArduinoJson 7.x |
| `ArduinoJson 7.x is required` | Update the library from version 6 |
| `Arduino-ESP32 core 3.x is required` | Update the esp32 board package |
| `This pin map is for the classic ESP32 only` | Select *ESP32 Dev Module* |
| `tx_dashboard.h` / `rx_dashboard.h` not found | Open the `.ino` inside its own folder |
| `GPIO6-11 ... must not be used` | A pin constant was changed to a forbidden GPIO |
| Type "does not name a type" after editing | A function was added above the type definitions. The Arduino builder inserts prototypes before the first function, so keep functions below the types |
| Error about `_currentClient`, `_currentStatus` | Another core version changed `WebServer`. Use 3.3.12 |

## Boot and upload

| Symptom | Cause and fix |
|---|---|
| Upload stops at "Connecting..." | Hold BOOT; check cable (data, not charge-only) and port |
| Reset loop, "Brownout detector" | Weak supply or cable. Use another, add bulk capacitance |
| RX resets with "task watchdog" | The output task stopped for more than 5 s. The reset is deliberate and leaves outputs OFF. Note the serial log |
| Green range LED flickers at power-up | Normal for GPIO14 before the firmware starts |
| Board does not boot with wiring attached | Nothing may be on GPIO12 or GPIO6-11 |

## Wi-Fi

| Symptom | Cause and fix |
|---|---|
| `esp_tx` not visible | Check TX serial for `[AP] started`. The hotspot is 2.4 GHz only. Try another `AP_CHANNEL` (1, 6, 11) |
| GPIO25 alternates blink and OFF | RX cannot join. TX is off or out of range, or the password differs |
| RX serial: reason 201 | Hotspot not found |
| RX serial: reason 15, 202 or 204 | Wrong password |
| Often drops | Distance, metal case, busy channel, weak supply |
| Phone leaves the network | It prefers networks with internet. Choose "stay connected" |
| RX cannot join, many phones connected | Limit is 6 stations (`AP_MAX_CLIENTS`) |

## Link and API

Read `last_result` on the RX dashboard or in `/api/status`.

| Value | Meaning and fix |
|---|---|
| `CONNECT_FAILED` | TX server not reachable. Open `http://192.168.4.1/api/data` on a phone |
| `HTTP_TIMEOUT` | TX too slow: close extra dashboards, improve signal, or raise `HTTP_TIMEOUT_MS` (then also `DATA_TIMEOUT_MS`) |
| `HTTP_ERROR` | Other transport problem, usually signal |
| `HTTP_STATUS` | Not HTTP 200. Another device may own 192.168.4.1 |
| `BAD_LENGTH`, `BAD_JSON`, `BAD_FIELDS` | TX and RX firmware do not match. Flash both from the same project version |
| `TX_FAULT` | TX sampling task stopped. Restart the TX, read its serial log |
| `STALE_SEQUENCE` | TX answers but its data is frozen. Restart the TX |

| Symptom | Cause and fix |
|---|---|
| RX shows TIMEOUT for a moment when a phone loads a page | One page load delayed polling. Rare events are harmless (outputs go OFF and return). A phone that walks away in the middle of a page load can hold the TX server for several seconds. If frequent, improve signal or raise `DATA_TIMEOUT_MS` |
| RX stays CONNECTED a few seconds after TX is unplugged | Normal: beacon loss takes about 3 s. Outputs are already OFF after 1 s |
| RX dashboard stopped opening | The DHCP address changed. Read the RX OLED |
| Page does not load at all | Use `http://`, switch off VPN and mobile data |

## OLED

| Symptom | Cause and fix |
|---|---|
| Blank, `oled_state` is `NOT_FOUND` | Check SDA=21, SCL=22, power, pin order. Try `OLED_ADDRESS` 0x3D. The firmware retries every 5 s without a restart |
| Shifted picture or noise at the edge | SH1106 controller: not supported by these sketches |
| Flicker or garbage | Long wires: shorten, or lower `OLED_I2C_HZ` to 100000; add pull-ups |

## Potentiometer and button

| Symptom | Cause and fix |
|---|---|
| Value jumps randomly | GPIO34 floating: check wiper and both ends |
| Never reaches 0 or 100 % | ESP32 ADC is not linear near the ends. Set `POT_RAW_MIN` / `POT_RAW_MAX` to the values you read |
| Runs backwards | Swap the two outer terminals |
| Last digit flickers | Fit C1, raise `POT_DEADBAND_RAW` |
| Switch always 1 | GPIO27 shorted to GND, or 4-pin button turned by 90° |
| Switch always 0 | Button not connected to GND |

## LEDs and buzzer

| Symptom | Cause and fix |
|---|---|
| No range LED at all | Link must be LIVE. The button is not needed |
| LED never lights | Reversed LED, missing GND, wrong GPIO |
| LEDs inverted | Active-low module: `LED_ACTIVE_HIGH = false` |
| Both green LEDs confused | GPIO14 = range, GPIO25 = communication |
| Buzzer silent | Check V_BUZ, polarity, Q1 pinout, common ground |
| Buzzer only clicks | It is passive. Use an active buzzer |
| Buzzer always on | Q1 wired wrong, or driver is active-low: `BUZZER_ACTIVE_HIGH = false` |
| Buzzer chirps at boot | R6 missing |
| No beep in yellow | `BUZZER_IN_YELLOW` must be `BuzzerMode::BEEP` |
