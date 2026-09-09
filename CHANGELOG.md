# Changes in 1.1.0

- Reconstructed the Word listing into named Arduino and C++ source files; removed merged comments, broken strings, invalid fragments and the `historyCount1` typo.
- Centralized all supplied pin mappings, intervals, thresholds, schedules and relay numbering.
- Preserved the existing access-point credentials for C3 compatibility.
- Applied ON, OFF and AUTO modes consistently to all mist makers, the fan, sprinkler and lights.
- Capped all sprinkler runs at 30 seconds; repeated ON does not extend a running cycle.
- Persisted the scheduled sprinkler date before energizing the output to prevent repeat watering after reboot in the scheduled minute.
- Restored live dashboard sensor rows and polls; relay controls read their state from the controller.
- Accepted only known sensor IDs, finite temperature/humidity values, and bounded complete spectral triples.
- Implemented calendar validation, lost-power handling and verified RTC synchronization; removed the unneeded fixed two-second schedule adjustment.
- Used the commanded light state for spectral collection, including manual OFF during scheduled hours.
- Made stale checks wrap-safe and retained true readings rather than replacing stale observations with invented zero values.
- Preserved the shared C3 recovery relay, startup grace and retry cooldown.
- Captured mist-maker status at observation time for temperature CSV rows.
- Corrected spectral output ordering and preserved the existing treatment columns.
- Added incremental, verified two-file export with checksums, commit markers and retry; pending data is retained on SD failures.
- Protected frozen export snapshots from incoming updates and counted buffer omissions instead of silently evicting old readings.
- Kept all display hardware assumptions explicit. No C3 sensor firmware, spectral calibration, or simulated measurements were invented.
- Added portable control/history/storage regression tests and setup/API/logging documentation.
