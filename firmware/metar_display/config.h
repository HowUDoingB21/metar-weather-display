#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ── Data URLs (GitHub Pages) ──────────────────────────────────────────────────
#define DATA_URL   "https://howudoingb21.github.io/metar-weather-display/data.json"
#define RADAR_URL  "https://howudoingb21.github.io/metar-weather-display/radar.jpg"

// ── Time ──────────────────────────────────────────────────────────────────────
#define NTP_OFFSET_SEC  -21600L   // UTC-6 (CST, Mexico City — no DST since 2022)

// ── Update interval ───────────────────────────────────────────────────────────
#define UPDATE_INTERVAL_MS  300000UL   // 5 minutes
