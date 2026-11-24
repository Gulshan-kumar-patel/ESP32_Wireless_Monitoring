# Installation

## 1. Software versions

| Item | Version used to compile | Accepted |
|---|---|---|
| Arduino IDE | 2.x (arduino-cli 1.6.0) | 2.x |
| Board package `esp32` by Espressif Systems | 3.3.12 | 3.x |
| ArduinoJson (Benoit Blanchon), RX only | 7.4.3 | 7.x |
| Adafruit GFX Library | 2.0.0 | current |
| Adafruit SSD1306 | 2.5.17 | current |
| Adafruit BusIO (dependency) | 1.17.4 | current |

`WiFi`, `WebServer`, `HTTPClient` and `Wire` come with the board package. The
sketches stop with a clear error on core 2.x, on ArduinoJson 6 (RX) and on
other chips such as the ESP32-S3 or C3. Other 3.x versions were not compiled.
The sketches use protected members of `WebServer`, so a future core could
require a small change.

## 2. Board package

1. *File > Preferences > Additional boards manager URLs*: add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   (recent IDE versions already list the package without it).
2. *Tools > Board > Boards Manager*: install **esp32 by Espressif Systems** 3.3.12.

## 3. Libraries

*Tools > Manage Libraries*, install:

1. **ArduinoJson** by Benoit Blanchon, 7.x
2. **Adafruit GFX Library**
3. **Adafruit SSD1306** (accept the Adafruit BusIO dependency)

## 4. Board settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module (or DOIT ESP32 DEVKIT V1) |
| Partition scheme | Default 4MB with spiffs |
| Upload speed | 921600, or 115200 if uploads fail |
| Port | The board's COM port |

If no port appears, install the USB driver for the board's chip (CP210x or CH340).

## 5. Flash

1. Open `transmitter/transmitter.ino`, select the TX board's port, upload.
2. Open `receiver/receiver.ino`, select the RX board's port, upload.

Open the `.ino` from inside its own folder so that the dashboard header is found.
If the upload waits at "Connecting...", hold **BOOT** until it starts.
Serial Monitor: 115200 baud. Set `DEBUG_SERIAL` to 0 to remove serial output.

Command line alternative:

```text
arduino-cli compile --fqbn esp32:esp32:esp32 transmitter
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM5 transmitter
arduino-cli compile --fqbn esp32:esp32:esp32 receiver
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM6 receiver
```

## 6. First start

1. Power the TX. Its OLED shows AP ON, SSID and `192.168.4.1`.
2. Power the RX. The GPIO25 LED blinks, then stays ON. The OLED shows its IP.
3. Connect a phone or laptop to `esp_tx`, password `esp32@2027`. Stay connected
   if it warns that there is no internet (on phones, switch mobile data off if
   pages do not open).
4. Open `http://192.168.4.1/` for the TX dashboard.
5. Open `http://<receiver-ip>/`, using the IP from the RX OLED. It usually
   starts at `192.168.4.2` but can change after a restart.

Type `http://` explicitly: some browsers try HTTPS first.

## 7. Acceptance test

| Test | Expected |
|---|---|
| Power TX | `esp_tx` appears, TX OLED complete |
| Power RX | Joins by itself, OLED shows RX IP and LIVE |
| Turn the potentiometer end to end | 0 to 100 % on both dashboards |
| Press and release the button | `switch` 1 then 0 |
| Potentiometer at 0-33 % | Only green range LED, buzzer silent |
| Potentiometer at 34-66 % | Only yellow LED, short beep each second |
| Potentiometer at 67-100 % | Only red LED, continuous buzzer |
| Press or release the button | Range LEDs and buzzer do not change |
| GPIO25 LED during all of the above | Steady ON |
| Hold button in red range, unplug TX | Outputs OFF within about 1 s, TIMEOUT, then DISCONNECTED |
| Plug TX in again | RX returns to LIVE by itself |
| Restart RX | Outputs OFF during boot, then LIVE |
| Disconnect the phone | Dashboard shows Offline |
| Unplug an OLED | Everything else keeps working |
