# Pinout

This is the authoritative GPIO map. The same values are in the `CONFIGURATION`
section at the top of each sketch. Board: ESP32-WROOM-32 / DevKit V1.

## Transmitter (TX)

| Component | Signal | GPIO | Firmware constant | Direction |
|---|---|---|---|---|
| OLED | SCL / SCK | 22 | `PIN_OLED_SCL` | I2C |
| OLED | SDA | 21 | `PIN_OLED_SDA` | I2C |
| 10 kΩ potentiometer | Wiper | 34 | `PIN_POT` | Analog input (ADC1_CH6) |
| Push button | To GND | 27 | `PIN_BUTTON` | `INPUT_PULLUP` |

- Pressed = LOW on the pin = `"switch":1`. Released = HIGH = `"switch":0`.
- GPIO34 is input-only and has no internal pull-up or pull-down.
- GPIO34 belongs to ADC1, which keeps working while Wi-Fi is on (ADC2 does not).

## Receiver (RX)

| Component | Signal | GPIO | Firmware constant | Direction |
|---|---|---|---|---|
| OLED | SCL / SCK | 22 | `PIN_OLED_SCL` | I2C |
| OLED | SDA | 21 | `PIN_OLED_SDA` | I2C |
| Red range LED (67-100 %) | Anode via resistor | 13 | `PIN_LED_RED` | Output |
| Green range LED (0-33 %) | Anode via resistor | 14 | `PIN_LED_GREEN` | Output |
| Yellow range LED (34-66 %) | Anode via resistor | 26 | `PIN_LED_YELLOW` | Output |
| Green communication LED | Anode via resistor | 25 | `PIN_LED_COMM` | Output |
| Buzzer driver | Transistor base/gate via resistor | 27 | `PIN_BUZZER` | Output |

GPIO25 (communication) and GPIO14 (green range) are two different LEDs.

## Pins that must not be used here

| GPIO | Reason |
|---|---|
| 6-11 | Connected to the SPI flash on ESP32-WROOM-32 modules |
| 12 | Boot-strapping pin: a wrong level at reset can prevent booting |

Both sketches contain a compile-time check that rejects these pins.

## Boot behaviour to expect

Between reset and the first line of `setup()` the firmware has no control.
GPIO14 is a JTAG pin (MTMS) with an internal pull-up or other activity during
this time, so the green range LED may glow or flicker briefly at power-up.
The buzzer stays silent because R6 (10 kΩ to GND) holds its transistor off.
