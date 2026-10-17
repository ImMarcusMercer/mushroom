# MushroomFarm Schematics and Wiring

This document describes the current ESP-only test architecture.

## Hardware roles

- **Main:** ZY-ESP32 / classic ESP32
- **Nodes:** ESP32-C3 SuperMini
- **Current active sensors per node:**
  - 3 x VEML7700 lux sensor
  - 1 x DHT11 temperature/humidity sensor
- **Moisture:** code and reserved pins retained, but sensing is disabled
- **Future target per node:** 9 x VEML7700 + 3 x moisture sensors

---

## 1. System architecture

```text
                                      ┌───────────────────────────────┐
                                      │        ZY-ESP32 MAIN          │
                                      │                               │
                                      │ Wi-Fi AP + HTTP Server        │
                                      │ 192.168.4.1                   │
                                      │                               │
                                      │ TFT Display                   │
                                      │ SD Card                       │
                                      │ DS3231 RTC                    │
                                      │ MCP23017 Relay Expander       │
                                      └───────────────┬───────────────┘
                                                      │
                                             Wi-Fi / HTTP POST
                                                      │
                    ┌─────────────────────────────────┼─────────────────────────────────┐
                    │                                 │                                 │
          ┌─────────▼─────────┐             ┌─────────▼─────────┐             ┌─────────▼─────────┐
          │ ESP32-C3 Node 1   │             │ ESP32-C3 Node 2   │             │ ESP32-C3 Node 3   │
          │ ID: sensor1       │             │ ID: sensor2       │             │ ID: sensor3       │
          │                   │             │                   │             │                   │
          │ TCA9548A          │             │ TCA9548A          │             │ TCA9548A          │
          │ 3 VEML7700 now    │             │ 3 VEML7700 now    │             │ 3 VEML7700 now    │
          │ 1 DHT11 now       │             │ 1 DHT11 now       │             │ 1 DHT11 now       │
          │ moisture disabled │             │ moisture disabled │             │ moisture disabled │
          └───────────────────┘             └───────────────────┘             └───────────────────┘
```

Nodes do not connect to Main with UART, SPI, or I2C. Their data path is Wi-Fi.

---

## 2. ZY-ESP32 Main wiring

The Main wiring below preserves the legacy mapping.

### TFT display - VSPI

| TFT signal | ZY-ESP32 pin |
|---|---:|
| MOSI / SDI | GPIO23 |
| MISO / SDO | GPIO19 |
| SCK / CLK | GPIO18 |
| CS | GPIO5 |
| GND | GND |
| VCC | Match the existing TFT module requirement |

### TFT DC and RESET

The previous code/document does not specify the TFT controller, `DC`, or `RST` pin numbers. `TFT_eSPI` normally keeps those in its `User_Setup` configuration.

Preserve the TFT_eSPI configuration used by the existing working Main hardware.

### SD card - HSPI

| SD signal | ZY-ESP32 pin |
|---|---:|
| MOSI | GPIO13 |
| MISO | GPIO12 |
| SCK | GPIO14 |
| CS | GPIO15 |
| GND | GND |
| VCC | Match the actual SD module |

The Main intentionally uses:

```text
TFT -> VSPI
SD  -> HSPI
```

### DS3231 RTC - I2C

| DS3231 | ZY-ESP32 |
|---|---:|
| SDA | GPIO21 |
| SCL | GPIO22 |
| GND | GND |
| VCC | Appropriate module supply |

### MCP23017 - I2C

MCP23017 shares the Main I2C bus with the DS3231.

| MCP23017 | ZY-ESP32 |
|---|---:|
| SDA | GPIO21 |
| SCL | GPIO22 |
| GND | GND |
| VCC | Appropriate supply |
| I2C address | `0x20` |

### MCP23017 relay mapping

| MCP23017 pin | Function |
|---|---|
| GPA0 | Mist Maker 1 |
| GPA1 | Mist Maker 2 |
| GPA2 | Mist Maker 3 |
| GPA3 | Exhaust Fan |
| GPA4 | Sprinkler Pump |
| GPA5 | Lights |
| GPA6 | Unused |
| GPA7 | ESP32-C3 node-power relay |

The firmware assumes active-low relay inputs:

```text
LOW  -> relay energized
HIGH -> relay released
```

Verify the real relay module before connecting high-voltage/high-current equipment.

### Main display-mode button

| Connection | ZY-ESP32 |
|---|---:|
| Button side 1 | GPIO33 |
| Button side 2 | GND |

The code uses:

```cpp
pinMode(BUTTON_PIN, INPUT_PULLUP);
```

---

## 3. ESP32-C3 SuperMini Node pin map - current test

The same sensor pin mapping is used on Nodes 1-3. Only the node ID changes.

```text
Node 1 -> sensor1
Node 2 -> sensor2
Node 3 -> sensor3
```

### Current pin allocation

| Function | ESP32-C3 SuperMini pin |
|---|---:|
| I2C SDA for TCA9548A | GPIO4 |
| I2C SCL for TCA9548A | GPIO5 |
| DHT11 DATA | GPIO6 |
| Moisture 1 reserved | GPIO0 |
| Moisture 2 reserved | GPIO1 |
| Moisture 3 reserved | GPIO3 |

Moisture pins are reserved only. They are not read while:

```cpp
const bool ENABLE_MOISTURE = false;
```

---

## 4. TCA9548A wiring

### TCA9548A #1 - current multiplexer

| TCA9548A #1 | ESP32-C3 SuperMini |
|---|---:|
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO4 |
| SCL | GPIO5 |
| A0 | GND |
| A1 | GND |
| A2 | GND |

With A0/A1/A2 LOW:

```text
I2C address = 0x70
```

### Current VEML7700 connections

#### Lux sensor 1

| VEML7700 #1 | TCA9548A #1 CH0 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | SD0 |
| SCL | SC0 |

#### Lux sensor 2

| VEML7700 #2 | TCA9548A #1 CH1 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | SD1 |
| SCL | SC1 |

#### Lux sensor 3

| VEML7700 #3 | TCA9548A #1 CH2 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | SD2 |
| SCL | SC2 |

Why the multiplexer is required:

```text
All VEML7700 sensors use the same I2C address.
```

Putting each sensor on a separate TCA channel allows the ESP32-C3 to select one sensor at a time.

Current lux topology:

```text
ESP32-C3 GPIO4 SDA ─────┐
ESP32-C3 GPIO5 SCL ───┐ │
                       │ │
                       ▼ ▼
                 TCA9548A #1
                   address 0x70
                       │
              ┌────────┼────────┐
              │        │        │
             CH0      CH1      CH2
              │        │        │
             LUX1     LUX2     LUX3
```

---

## 5. DHT11 wiring - active now

The current test uses one DHT11 per node.

### If using a 3-pin DHT11 module

| DHT11 module | ESP32-C3 SuperMini |
|---|---:|
| VCC / + | 3.3 V |
| DATA / OUT | GPIO6 |
| GND / - | GND |

### If using a bare 4-pin DHT11

Typical bare-sensor wiring is:

| Bare DHT11 | Connection |
|---|---|
| VCC | 3.3 V |
| DATA | GPIO6 |
| NC | no connection |
| GND | GND |

A bare DHT11 normally requires a pull-up resistor from DATA to 3.3 V. A typical value is around 10 kOhm. Many 3-pin DHT11 modules already contain this pull-up.

Do not pull the DHT data line to 5 V when it is connected directly to an ESP32-C3 GPIO.

---

## 6. Complete current Node test wiring

```text
                           ESP32-C3 SuperMini
                        ┌─────────────────────┐
              3.3 V ───┤ 3V3                 │
              GND ─────┤ GND                 │
                        │                     │
TCA9548A SDA ──────────┤ GPIO4               │
TCA9548A SCL ──────────┤ GPIO5               │
DHT11 DATA ────────────┤ GPIO6               │
                        │                     │
Moisture 1 future ─────┤ GPIO0               │
Moisture 2 future ─────┤ GPIO1               │
Moisture 3 future ─────┤ GPIO3               │
                        └─────────────────────┘
             │                         │
             │                         └──── DHT11
             │                               VCC  -> 3.3 V
             │                               DATA -> GPIO6
             │                               GND  -> GND
             │
             ▼
       TCA9548A #1 (0x70)
       ├── CH0 -> VEML7700 #1
       ├── CH1 -> VEML7700 #2
       └── CH2 -> VEML7700 #3
```

---

## 7. Power and grounding

For one Node:

```text
ESP32-C3
TCA9548A
VEML7700 #1
VEML7700 #2
VEML7700 #3
DHT11
```

must share a common ground.

```text
ESP32-C3 GND
  ├── TCA GND
  ├── VEML #1 GND
  ├── VEML #2 GND
  ├── VEML #3 GND
  └── DHT11 GND
```

Use 3.3 V logic for components connected directly to ESP32-C3 GPIO/I2C lines.

Avoid powering the complete sensor network from a weak 3.3 V source if the regulator cannot provide enough current. For the bench test, use a stable supply and keep wiring short.

---

## 8. Future 9-lux expansion

The Node firmware already contains routes for nine VEML7700 devices.

### TCA9548A #1

```text
Address: 0x70
CH0 -> Lux1
CH1 -> Lux2
CH2 -> Lux3
CH3 -> Lux4
CH4 -> Lux5
CH5 -> Lux6
CH6 -> Lux7
CH7 -> Lux8
```

### TCA9548A #2

Configure a second TCA9548A as address `0x71` by changing its address pins appropriately.

```text
Address: 0x71
CH0 -> Lux9
```

Both multiplexers share the same upstream ESP32-C3 I2C bus:

```text
GPIO4 SDA -> TCA #1 SDA + TCA #2 SDA
GPIO5 SCL -> TCA #1 SCL + TCA #2 SCL
```

The multiplexer addresses must be different.

---

## 9. Future moisture wiring - disabled now

The code reserves:

| Future sensor | ESP32-C3 ADC pin |
|---|---:|
| Moisture #1 | GPIO0 |
| Moisture #2 | GPIO1 |
| Moisture #3 | GPIO3 |

Current firmware setting:

```cpp
const bool ENABLE_MOISTURE = false;
```

Do not connect or depend on these moisture channels for the current Node 1 -> Main test.

When moisture sensing is eventually enabled, verify the exact sensor type and make sure its analog output never exceeds the ESP32-C3 ADC input voltage limit. Calibration should be performed before converting raw ADC values into a percentage.

---

## 10. Current network/data flow

```text
VEML7700 #1 ─┐
VEML7700 #2 ─┼─> TCA9548A ─> ESP32-C3 Node
VEML7700 #3 ─┘                    │
                                  ├─ reads Lux1/Lux2/Lux3
DHT11 ───────────────────────────>├─ reads Temp/Humidity
                                  │
                                  ├─ builds JSON
                                  │
                                  └─ Wi-Fi HTTP POST
                                           │
                                           ▼
                                     ZY-ESP32 Main
                                           │
                              ┌────────────┼────────────┐
                              │            │            │
                             TFT        Web UI         SD
```

Current payload example:

```json
{
  "id": "sensor1",
  "seq": 42,
  "uptime_ms": 210345,
  "temp": 27.0,
  "hum": 78.0,
  "lux": [123.45, 118.90, 131.22],
  "moisture_raw": []
}
```

This is the expected format for the current bench test.


## Visual diagram

Updated system diagram image included in this ZIP: `VISUALS_1.2.png`.
