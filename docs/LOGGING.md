# CSV logging and recovery

The controller keeps up to 800 records in RAM and requests a CSV export once per hour, starting the timer on the first valid sensor packet. `LOG_NOW` and the dashboard save button request an earlier export. The two CSV schemas retain the original column names.

## Temperature and humidity

```csv
Date,Time,Sensor,Temperature,Humidity,Status
09/09/2026,05:00:10 PM,sensor1,25.40,74.20,ON
```

Within each five-minute window, the latest temperature/humidity observation replaces the earlier observation for that sensor. `Status` is that sensor's mist-maker output state at the time the retained observation was processed, not its state at the later hourly export. This is not an event log of every relay transition.

## Spectrometer treatments

```csv
Date,Time,Treatment1,Treatment2,Treatment3,Treatment4,Treatment5
09/09/2026,05:00:10 PM,"505 506 507 508 509 510","520 521 522 523 524 525","","",""
```

The first three accepted sample sets per treatment per five-minute window are retained. Each set has three values, so a treatment cell contains up to nine space-separated integers. Ordering is complete first set, then complete second set, then complete third set. Empty treatments are represented by an empty quoted cell. The examples are illustrative data only.

No retransmission identifier is present in the original sensor protocol. Therefore repeated packets, including a sender retry after an uncertain response, count as additional sample sets until the three-set cap is reached. Identical values are not assumed to be duplicates because real consecutive samples may be equal. Keep sending temperature/humidity heartbeats more frequently than the stale timeout even after the spectral cap is reached.

## Verified export transaction

1. Freeze the pending prefix of history. New readings can continue accumulating in the remaining space; frozen records are never changed while the export is pending.
2. Create uniquely named temporary temperature and spectrometer files under `/logs`. Names include the first record's date/time, a random boot identifier, and an attempt sequence.
3. Write rows incrementally, returning to the main loop between rows so it can serve requests and recheck control deadlines.
4. Read both files back and verify their byte counts and CRC32 checksums.
5. Rename both to `.csv`. Create and verify a small commit marker, then rename it to the shared basename plus `.ok`.
6. Only after that final marker rename succeeds, release the frozen records from RAM.

The marker records the export version, record count, file byte counts and CRC32 values. A completed export has matching `-temp.csv`, `-spectro.csv` and `.ok` names. Ignore unfinished `.tmp` files and CSV pairs without a matching completed marker when collecting experimental data. Files left by an abrupt power loss are not automatically erased on the next boot. The project never overwrites existing log files.

Write, read-back, checksum or rename failure retains the frozen records and retries after 30 seconds. SD remount is also retried. The dashboard shows the error and pending counts. A logical snapshot can cross a clock boundary or an explicit `LOG_NOW`; later records in the same five-minute window may then appear in the next export. Sample quotas remain shared across exports, and previously frozen records are never rewritten.

## Capacity and power loss

At the original full sampling rate, 3 DHT records plus 15 spectral-set records per five-minute window equals 216 records per hour. An 800-record buffer is approximately 3.7 hours at that rate, not a month. Lights-off periods consume fewer records. Frequent manual exports, partial windows, or clock adjustments can change the effective capacity.

If the buffer fills during an extended SD failure, already-buffered records are retained. New history records that cannot fit are counted as omitted and shown on the dashboard; fresh measurements still update the live controls. There is no claim of unlimited retention. RAM history is not restored after a main-controller power loss. Completed, verified SD exports remain on the card.

If the RTC is unavailable, dates are `No_Date`, times are prefixed `up_`, and filenames use uptime. The firmware does not backfill fabricated dates after synchronization. Window keys separate valid clock time, uptime, and clock-correction generations, and include the full epoch rather than day-of-year alone.

SD calls can still block while the hardware driver waits. Incremental logging reduces long application loops, but does not turn a slow or failing SD card into a hard real-time peripheral. This release adds no automatic SD deletion or retention policy.
