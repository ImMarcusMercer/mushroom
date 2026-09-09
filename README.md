# MushroomFarm Monitor 1.1.0

This project revises the main ESP32 controller in `Mushroom Code1.docx` into an Arduino sketch with separate C++ source and header files. It receives the existing C3 sensor messages, operates the farm relays, displays readings, serves a local dashboard, and exports temperature/humidity and treatment CSV files.

Start with [docs/SETUP.md](docs/SETUP.md). Open `MushroomFarmMonitor/MushroomFarmMonitor.ino` in Arduino IDE; keep all files in that sketch folder together.

## Included files

| File | Purpose |
| --- | --- |
| `MushroomFarmMonitor.ino` | Arduino `setup()` and `loop()` entry points |
| `Firmware.cpp` and `Firmware.h` | ESP32 peripherals, HTTP endpoints, serial commands, TFT and main loop |
| `Config.h` | Pins, thresholds, schedules, history capacity and recovery timings |
| `NetworkConfig.h` | Original Wi-Fi credentials and optional HTTP credentials |
| `ControlCore.cpp` and `.h` | Sensor validation, automatic/manual modes, schedules and C3 power recovery |
| `History.cpp` and `.h` | Five-minute aggregation and protected history snapshots |
| `LogWriter.cpp` and `.h` | CSV generation, read-back verification and commit/retry handling |
| `Dashboard.h` | Embedded HTML, CSS and JavaScript; no separate web hosting needed |
| `hardware/TFT_User_Setup.template.h` | Display configuration template for the hardware details missing from the document |
| `docs/API.md`, `docs/LOGGING.md` | Sender protocol, controls and CSV behavior |
| `tests/` and `tools/run_tests.py` | Portable C++ regression and storage-failure tests |
| `tools/build.py` | Optional Arduino CLI dependency installation and compilation |

## Preserved configuration

| Device | Automatic behavior |
| --- | --- |
| Mist makers 1–3 | ON below 75% RH, OFF at or above 85% RH; hold state between thresholds |
| Exhaust fan | ON if any fresh sensor is above 29°C or below 22°C |
| Sprinkler | Once during the 2:00 PM minute, for 30 seconds |
| Lights | ON 4:00 PM through 6:59:59 AM, OFF from 7:00 AM |
| CSV export | Every hour; observations grouped into five-minute windows |
| C3 power recovery | 30-second stale threshold, 60-second startup grace, 5-second shared power cut, 60-second cooldown after restoring power |

The outside-range exhaust rule, including operation below 22°C, is preserved from the original executable intent. `EXHAUST_ON_LOW_TEMPERATURE` can disable its low-temperature branch. The original comments referring to 84/86% humidity, 10-second sprinkling, an 8:30 AM sprinkler run, and 5 AM–8 PM lighting were inconsistent with the actual constants and functions.

Manual ON/OFF works for all six outputs and persists until AUTO is selected, overrides are reset, the controller restarts, or the relay expander is reinitialized. Manual ON can keep a mist maker running without a fresh sensor; AUTO shuts mist makers off when their sensor becomes stale. The sprinkler always has a 30-second run cap; after a manual run times out it stays in manual OFF until another command. Repeating ON while a pump run is already active does not extend that run.

## Hardware details still required

The document supplies neither the display controller model nor TFT DC/RST wiring. Use the existing working TFT_eSPI setup or fill in the template using the actual hardware. The firmware checks the supplied VSPI pins and 480×320 geometry and leaves the display disabled if they do not match. The rest of the controller continues operating.

The ESP32-C3 acquisition firmware was not supplied. This package contains the main ESP32 controller; [docs/API.md](docs/API.md) describes the messages the C3 nodes must send. No spectrometer measurements are fabricated, and no sensor driver or calibration is guessed.

## Validation

See [docs/VALIDATION.md](docs/VALIDATION.md) for the checks performed on this release and the hardware checks that remain. No ready-to-flash binary is included because the TFT hardware setup is specific to your installation.
