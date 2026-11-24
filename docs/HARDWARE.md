# Hardware

Drawings are in `../schematics/`:

- `transmitter_schematic.png`
- `receiver_schematic.png`
- `wiring_diagram.png` (both boards, DevKit V1 pin positions)

They were drawn as vector graphics and exported to PNG. They are not exported
from an EDA tool, so there is no netlist and no electrical rule check.

## Transmitter parts

| Ref | Part | Qty | Notes |
|---|---|---|---|
| U1 | ESP32-WROOM-32 / DevKit V1 | 1 | USB powered |
| DS1 | 0.96 in OLED, SSD1306, 128×64, I2C | 1 | Address `0x3C` assumed |
| RV1 | 10 kΩ potentiometer, linear | 1 | Ends to 3.3 V and GND |
| SW1 | Momentary push button, normally open | 1 | Between GPIO27 and GND |
| C1 | 100 nF ceramic | 1 | Optional, GPIO34 to GND, reduces ADC noise |

## Receiver parts

| Ref | Part | Qty | Notes |
|---|---|---|---|
| U2 | ESP32-WROOM-32 / DevKit V1 | 1 | USB powered |
| DS2 | 0.96 in OLED, SSD1306, 128×64, I2C | 1 | Address `0x3C` assumed |
| D1 | Red LED | 1 | GPIO13 |
| D2 | Green LED | 1 | GPIO14, range |
| D3 | Yellow LED | 1 | GPIO26 |
| D4 | Green LED | 1 | GPIO25, communication |
| R1-R4 | 220 Ω (220-330 Ω) | 4 | One per LED |
| BZ1 | Active buzzer | 1 | Model not specified, see below |
| Q1 | NPN transistor or logic-level N-MOSFET | 1 | Selected from the BZ1 datasheet |
| R5 | 1 kΩ (NPN) or about 100 Ω (MOSFET gate) | 1 | Selected from the BZ1 current |
| R6 | 10 kΩ | 1 | Base/gate to GND, holds Q1 off during boot |
| D5 | Flyback diode | 0-1 | Only if BZ1 is magnetic/inductive |

## Power

- Power each board from its own stable 5 V USB supply.
- No GPIO may ever see more than 3.3 V. The potentiometer goes to 3.3 V, not 5 V.
- TX and RX need no common ground: the only link between them is Wi-Fi.
- If BZ1 has its own supply, connect that supply's ground to the RX ground.
- Place a 100 nF and a 10-100 µF capacitor near the 3.3 V pin if the board
  resets when Wi-Fi starts (a typical weak-supply symptom).

## OLED

- Check the module's supply range before connecting it. 3.3 V is assumed.
- **Check the pin order.** Some modules are GND-VCC-SCL-SDA, others
  VCC-GND-SCL-SDA. Swapping power destroys the module.
- Most modules carry SDA/SCL pull-ups. If yours does not, add 4.7 kΩ from each
  line to 3.3 V.
- If the module uses address `0x3D`, change `OLED_ADDRESS` in the sketch.

## LEDs

Active-high wiring: GPIO, resistor, LED anode; LED cathode to GND.

Current ≈ (3.3 V − V_F) / R. For a red LED with V_F = 2.0 V and 220 Ω this is
about 6 mA. Use your LED's datasheet value. Blue-chip ("pure green") LEDs have
V_F near 3 V and will be dimmer.

For active-low LED modules set `LED_ACTIVE_HIGH = false` in `receiver.ino`.

## Buzzer driver

The buzzer model, voltage and current are unknown, so the drawing shows a
generic low-side switch. Complete it as follows:

1. Read the rated voltage and current from the BZ1 datasheet.
2. V_BUZ = that rated voltage. Use the 3.3 V or 5 V (VIN) pin only if the
   voltage matches and the board can supply the current. Otherwise use a
   separate supply with common ground.
3. Choose Q1 with V_CE (or V_DS) and current ratings above those values.
   A MOSFET must be fully on at 3.3 V gate drive.
4. NPN base resistor: R5 ≤ (3.3 V − 0.7 V) / (I_buzzer / 10).
   With 1 kΩ, I_B ≈ 2.6 mA, which suits buzzer currents up to about 26 mA.
5. Fit D5 (cathode to V_BUZ) if BZ1 is magnetic. Piezo buzzers do not need it.

The firmware assumes an **active** buzzer (it sounds when DC is applied). A
passive buzzer needs a tone signal and will only click. For a driver that turns
on with a LOW level set `BUZZER_ACTIVE_HIGH = false`.

## Check before power-up

1. The board really is a classic ESP32 and exposes GPIO25 and GPIO26.
2. The OLED controller is SSD1306 (not SH1106), and its supply and pin order match.
3. LEDs are active-high.
4. The buzzer driver matches the buzzer datasheet.
5. No short between 3.3 V and GND.
