# HTTP interface and sensor messages

The controller is an access point at `192.168.4.1`, with HTTP on port 80. No internet connection or cloud backend is required. The existing SSID and password are in `NetworkConfig.h`.

## Sensor intake

Send `POST /data` with `Content-Type: application/json`:

```json
{"id":"sensor1","temp":25.4,"hum":79.2,"spectro":[505,506,507,520,521,522]}
```

These are format examples, not measured or calibrated reference values. The controller does not generate values for the C3 nodes.

| Field | Validation |
| --- | --- |
| `id` | Exactly `sensor1`, `sensor2`, or `sensor3` |
| `temp` | Required finite JSON number, from -40 to 80°C |
| `hum` | Required finite JSON number, from 0 to 100% |
| `spectro` | Optional; omit, use null or an empty array for no spectral sample. Otherwise signed 32-bit integers in complete groups of three. Sensor1 and sensor2 allow 3 or 6 values; sensor3 allows 3. |

Each JSON body is limited to 768 bytes after receipt. Invalid packets return 400 (or 413 for an empty/oversized body), do not update sensor freshness, and do not affect relay decisions. Success remains `200 OK` with the literal response body `OK`, matching the original receiver contract. The controller still accepts valid readings for live control if the history buffer is full; `/status` exposes a cumulative count of omitted historical records.

| Sender | Array entries | Treatment |
| --- | --- | --- |
| sensor1 | 0, 1, 2 | Treatment 1 |
| sensor1 | 3, 4, 5 | Treatment 2 |
| sensor2 | 0, 1, 2 | Treatment 3 |
| sensor2 | 3, 4, 5 | Treatment 4 |
| sensor3 | 0, 1, 2 | Treatment 5 |

Spectral values are displayed and added to history only while the commanded light output is ON and the relay expander is available. Manual OFF takes precedence over the lighting schedule. Temperature/humidity values are always accepted independently of lighting.

## Dashboard and controls

| Method and path | Request / result |
| --- | --- |
| `GET /` | Embedded dashboard |
| `GET /status` | JSON containing RTC, SD, sensors, relay modes/states, treatments, history usage and export errors |
| `POST /relay-control` | `{"relay":3,"action":"on"}`; actions are `on`, `off`, `auto` |
| `POST /reset-overrides` | Restore AUTO for every relay |
| `POST /sync-time` | Six integer fields: `year`, `month`, `day`, `hour`, `minute`, `second`; interpreted as Manila civil time |
| `POST /log-now` | Request export; 202 means queued, not successfully saved. Poll `/status` for completion or error. |

| HTTP relay number | Output |
| --- | --- |
| 1 / 2 / 3 | Mist Maker 1 / 2 / 3 |
| 4 | Exhaust fan |
| 5 | Sprinkler pump |
| 7 | Lights |

Number 6 remains unused to preserve the original control numbering. Relay control returns 503 if the MCP23017 is unavailable or its output write cannot be verified. `on` in the status payload represents the commanded output, not a physical relay-contact sensor. Interpret it together with `relays_available`.

## Optional access credentials

The original receiver allowed devices connected to its Wi-Fi network to access its HTTP endpoints. This remains the default for compatibility. Set a nonempty `WEB_PASSWORD` to require HTTP Basic authentication for the dashboard, status and manual controls. Set a nonempty `SENSOR_API_KEY` to require an `X-Sensor-Key` header on `/data`; update the C3 sender firmware before enabling that option. These are local HTTP interfaces, without TLS.
