# Release validation

Validation date: 9 September 2026. Release: 1.1.0.

## ESP32 compilation

Both configurations compiled and linked successfully for `esp32:esp32:esp32` using Espressif Arduino core 3.3.11 and the library versions in SETUP.md. Warnings were enabled.

| Build | Program storage | Static RAM | Result |
| --- | --- | --- | --- |
| TFT disabled | 1,070,725 / 1,310,720 bytes (81%) | 83,340 / 327,680 bytes (25%) | Passed, no compiler warnings |
| TFT enabled with a compile-only ILI9488 configuration | 1,080,921 / 1,310,720 bytes (82%) | 83,580 / 327,680 bytes (25%) | Passed; only TFT_eSPI's unused-touch warning |

The second build uses ILI9488, the documented VSPI pins, DC=2 and RST=-1 solely as a compiler configuration to exercise the TFT code path. Those missing hardware details were not inferred from the Word document and are not installed as the user's display configuration. Use the actual display model and wiring before uploading. The firmware uses a physical mode button, not a touch screen; TFT_eSPI's `TOUCH_CS` warning is expected in this test configuration.

Static RAM figures are linker estimates; they are not measurements of remaining heap while handling actual network requests. No binary from these compile-only configurations is included.

## Portable regression tests

All **24 scenarios** in `tests/test_core.cpp` passed with GCC C++17, `-Wall -Wextra -Werror -pedantic`, AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer was disabled because the validation runtime does not support its process inspection; heap-leak behavior is not claimed as tested.

The scenarios cover:

- Humidity hysteresis, exact threshold behavior, and the preserved high/low temperature exhaust rule.
- Mist Maker 3 manual overrides and return to automatic mode.
- Sensor timeout shutdown, readings received at millisecond zero, timer wraparound, and latching stale state until a new valid packet.
- Invalid sensor IDs, invalid/nonfinite readings, oversized arrays and protection from rejected updates.
- Scheduled pump duration, persisted-date restart deduplication and backward clock correction.
- Manual pump ON/OFF, repeated ON not extending a run, and timeout across timer wraparound.
- Missing-clock behavior, relay-hardware-unavailable state, overnight lighting and manual OFF.
- C3 recovery startup grace, power-cut duration and cooldown.
- Leap dates, invalid calendar dates, clock-domain and year-separated history windows.
- Temperature/humidity aggregation, the spectral three-set limit, snapshot immutability and lights-gated history.
- Full-buffer omission reporting without evicting old observations.
- Correct spectral CSV set order and preservation of records arriving during an export.
- Short writes, checksum corruption, SD disconnection, each CSV/marker rename failure, failed marker writes and subsequent successful retries.
- File-name collision protection and the standard CRC32 reference vector.

Run the tests locally with:

```sh
python tools/run_tests.py
```

On a compiler/runtime that supports sanitizers, use `python tools/run_tests.py --sanitize`.

## Other checks

The embedded dashboard JavaScript passed Node.js syntax validation. The Python build and test helpers passed syntax parsing. Interactive browser testing was not completed because a browser executable could not be obtained in the validation environment. The release archive was checked for file integrity and required project files.

## Hardware checks remaining

No physical ESP32, C3 node, relay module, RTC, SD card or display was attached during this revision. Compilation and host tests do not verify wiring, appliance behavior, SD-card electrical timing or actual sensor accuracy.

On the assembled system, check the TFT setup and button; sensor freshness and all five treatment mappings; ON/OFF/AUTO for each output; the 30-second pump cutoff; RTC synchronization and the daily run guard; and SD export/retry behavior with the actual card. Compare the dashboard's commanded states with the relay hardware. Confirm that C3 packet intervals are compatible with the 30-second stale threshold and that the shared recovery relay restores their power.
