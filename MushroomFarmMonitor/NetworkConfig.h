#pragma once
namespace farm {
// Filled from the supplied document so existing C3 nodes retain connectivity.
constexpr const char* AP_SSID = "MushrooMan";
constexpr const char* AP_PASSWORD = "RABE2026qt";
// Optional HTTP Basic authentication for the dashboard and control endpoints.
// Empty preserves the original LAN-only access model. /data is unaffected.
constexpr const char* WEB_USER = "admin";
constexpr const char* WEB_PASSWORD = "";
// Optional shared X-Sensor-Key header for /data. Leave empty for existing nodes.
constexpr const char* SENSOR_API_KEY = "";
}
