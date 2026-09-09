# Setup and upload

## Arduino IDE

1. Extract the ZIP completely. Open `MushroomFarmMonitor/MushroomFarmMonitor.ino`. Its containing folder and sketch name must remain `MushroomFarmMonitor`.
2. In Arduino IDE Preferences, add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` to Additional Boards Manager URLs.
3. Install **esp32 by Espressif Systems 3.3.11** using Boards Manager. Choose **ESP32 Dev Module** for a generic classic ESP32 board, then select the correct USB port. The main controller is a classic ESP32, not an ESP32-C3.
4. Install these libraries using Library Manager. Accept installation of their dependencies.

| Library | Version used for this project |
| --- | --- |
| ArduinoJson by Benoit Blanchon | 7.3.2 |
| TFT_eSPI by Bodmer | 2.5.43 |
| RTClib by Adafruit | 2.1.4 |
| Adafruit MCP23017 Arduino Library | 2.3.2 |
| Adafruit BusIO | 1.17.4 |

`WiFi`, `WebServer`, `SPI`, `Wire`, `SD`, `Preferences`, and the ESP timer support come with the ESP32 board package. Do not install unrelated libraries with the same names.

5. Read the TFT section below. Preserve your existing working TFT_eSPI configuration if the old display already worked.
6. Review `Config.h`. The original Wi-Fi SSID and password are retained in `NetworkConfig.h`, so already-configured C3 senders can reconnect. Keep that file private if publishing the project.
7. Compile using Verify, then upload. Open Serial Monitor at **115200 baud** with **Newline** line endings.
8. Connect a phone or laptop to the ESP32 access point. Open [the local dashboard](http://192.168.4.1). An unsynchronized but available RTC is synchronized once from that browser; the manual Sync Manila time button is also available.

## TFT configuration

TFT_eSPI compiles its display driver and pin selection into its own library sources. Defining pins only in the `.ino` does not configure those library sources.

If you have a working `TFT_eSPI/User_Setup.h`, retain it and its `User_Setup_Select.h` selection. Verify that it uses the actual display driver, the existing DC/RST wiring, and the VSPI pins in the table below. Leave `USE_HSPI_PORT` undefined because the SD card uses HSPI.

For a new installation, use `hardware/TFT_User_Setup.template.h` as a starting point. Select the actual controller, set the actual DC pin and RST connection, then copy the completed settings into the library's selected setup. The template intentionally stops compilation until the missing hardware fields are filled in. The source document only identifies a 4-inch 480×320 display; that is not sufficient to select a driver.

For testing without the display, change `MUSHROOM_ENABLE_TFT` to `0` in `Config.h`. This removes the TFT dependency from the build while retaining sensors, relays, logging and web control.

## Connections preserved from the document

| Connection | Classic ESP32 pin or expander pin |
| --- | --- |
| TFT MOSI / MISO / SCK / CS | GPIO23 / GPIO19 / GPIO18 / GPIO5 |
| TFT DC / RST | Not specified in the source; use actual wiring |
| SD MOSI / MISO / SCK / CS | GPIO13 / GPIO12 / GPIO14 / GPIO15 |
| RTC and MCP23017 SDA / SCL | GPIO21 / GPIO22 |
| Display mode button | GPIO33 to GND, internal pull-up enabled |
| DS3231 I2C address | 0x68 |
| MCP23017 I2C address | 0x20 |
| Mist Maker 1 / 2 / 3 | MCP23017 GPA0 / GPA1 / GPA2 |
| Exhaust fan | GPA3 |
| Sprinkler pump | GPA4 |
| Lights | GPA5 |
| Unused | GPA6 |
| Shared ESP32-C3 power relay | GPA7; normally closed contact supplies the nodes when the relay is de-energized |

Relay outputs are active LOW, as in the original program. The MCP23017 latches are loaded HIGH before outputs are enabled. The program checks expander communication and output pin levels; it cannot determine whether a relay contact or connected appliance physically switched.

All three expected nodes must send a valid temperature/humidity heartbeat more often than every 30 seconds. For example, a 10-second sender interval provides margin. Spectrometer samples can accompany those packets when available. If a node deliberately sleeps longer than 30 seconds, adjust `SENSOR_TIMEOUT_MS` and recovery settings to match it; otherwise the shared recovery relay will periodically interrupt all three nodes.

## Clock and sprinkler behavior

The DS3231 holds Manila wall-clock time (UTC+8). Browser synchronization converts from the browser's current timestamp into Manila time; no extra two-second schedule offset is applied.

A missing RTC, invalid calendar, or RTC lost-power flag disables automatic time schedules until a valid time is available. Temperature/humidity control and manual commands continue. A scheduled sprinkler run is recorded in ESP32 Preferences before its output is energized. This prevents another scheduled run after a reboot in the same 2:00 PM minute. If the main board was off for that whole minute, there is no catch-up watering. Moving the RTC backward conservatively blocks scheduled watering until its calendar date exceeds the last recorded run date. Manual timed watering remains available.

## Optional command-line build

Install Arduino CLI and Python 3, then run from the extracted project directory:

```sh
python tools/build.py --install
```

This installs the pinned dependencies and compiles using the selected TFT_eSPI setup. To compile without TFT support:

```sh
python tools/build.py --headless
```

The script compiles only; upload from Arduino IDE after selecting the physical board and port.

## Serial commands

| Command | Action |
| --- | --- |
| `LOG_NOW` | Request a verified export of pending readings |
| `BUFFER_STATUS` | Show pending/frozen history, omission count, heap, and logging errors |
| `LIST_FILES` | List SD files under `/logs` |
| `SET_TIME YYYY MM DD HH MM SS` | Validate and set Manila civil time |
| `RESET_OVERRIDES` | Return all outputs to automatic control |
| `HELP` | Print commands |

`LOG_NOW` does not manufacture test sensor readings. Send real sensor packets first.

## Reference documentation

- [Espressif Arduino ESP32 installation](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
- [TFT_eSPI setup selection](https://github.com/Bodmer/TFT_eSPI/blob/master/User_Setup_Select.h)
- [ArduinoJson 7 deserialization](https://arduinojson.org/v7/tutorial/deserialization/)
- [Adafruit MCP23017 library](https://github.com/adafruit/Adafruit-MCP23017-Arduino-Library)
- [Adafruit RTClib](https://github.com/adafruit/RTClib)
