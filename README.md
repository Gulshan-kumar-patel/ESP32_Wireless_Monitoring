# ESP32_Wireless_Monitoring

Two ESP32 boards linked by local Wi-Fi. The transmitter reads a potentiometer
and a push button. The receiver shows the value on LEDs and a buzzer. Each board
has an OLED and its own web dashboard. No internet, router or cloud is needed.

## Features

- TX hotspot `esp_tx`, HTTP + JSON API at `http://192.168.4.1`
- RX joins and rejoins automatically, receives every sample (20 ms) over UDP, HTTP polling as backup
- Range LEDs: green 0-33 %, yellow 34-66 %, red 67-100 %, always follow the potentiometer
- Buzzer: silent, short beep, continuous, follows the range LED
- Separate green communication LED on GPIO25
- All outputs OFF at boot, on Wi-Fi loss, on invalid data, and when data is
  older than 1000 ms
- Output control runs in its own watchdog-supervised task
- Offline dashboards for phone and desktop

## Quick start

1. Wire both boards: [PINOUT.md](PINOUT.md), [HARDWARE.md](HARDWARE.md), `../schematics/`.
2. Install core and libraries, flash `transmitter/` and `receiver/`:
   [INSTALLATION.md](INSTALLATION.md).
3. Join `esp_tx` with password `esp32@2027`.
4. Open `http://192.168.4.1/` (TX) and `http://<receiver-ip>/` (RX, IP on its OLED).

## Contents

```text
ESP32_Wireless_Monitoring/
├── transmitter/   transmitter.ino, tx_dashboard.h
├── receiver/      receiver.ino, rx_dashboard.h
├── docs/          README, TDS, HARDWARE, PINOUT, COMMUNICATION,
│                  INSTALLATION, TROUBLESHOOTING
└── schematics/    transmitter_schematic.png, receiver_schematic.png,
                   wiring_diagram.png
```

All settings are in the `CONFIGURATION` section at the top of each sketch.

## Build and test report (24 November 2026)

Toolchain: arduino-cli 1.6.0, board `esp32:esp32:esp32`, core 3.3.12,
ArduinoJson 7.4.3, Adafruit GFX 2.0.0, SSD1306 2.5.17, BusIO 1.17.4,
`--warnings all`.

| Check | Result |
|---|---|
| Compile transmitter | Pass, no warnings. Flash 979,152 bytes (74 %), static RAM 49,824 bytes (15 %) |
| Compile receiver | Pass, no warnings. Flash 1,017,004 bytes (77 %), static RAM 52,448 bytes (16 %) |
| Compile both with `DEBUG_SERIAL 0` | Pass, no warnings |
| Decision logic (range, buzzer, beep, link, communication LED, ADC scaling), extracted from the sketches and run on a PC | Pass, 1,897,471 checks |
| - stale, untrusted or missing data forces outputs OFF | Pass |
| - switch OFF forces range LEDs and buzzer OFF | Superseded: LEDs and buzzer now ignore the switch. Not re-run on a PC |
| - GPIO25 logic independent of switch and percentage | Pass |
| Dashboards in Edge against a mock API: TX online, invalid JSON, offline; RX live, timeout, connecting, HTTP 500; 360/390 px phone widths | Pass, inspected by eye |
| GPIO table against code, documents and drawings | Pass |
| No GPIO6-11 and no GPIO12 | Pass, enforced at compile time |
| JSON keys and routes against code, dashboards, documents | Pass |

**Not verified:** nothing has run on real ESP32 hardware. That includes Wi-Fi
joining and recovery, RX serving its dashboard while joined as a station, request
latency and the timing targets, the packet validation code at run time
(compiled only), OLED output, ADC quality, debounce feel, watchdog reset, long-run heap
stability, and every electrical circuit. The buzzer driver is generic because
the buzzer is unknown. The drawings have had no EDA rule check. Run the
acceptance test in [INSTALLATION.md](INSTALLATION.md) before relying on it.
