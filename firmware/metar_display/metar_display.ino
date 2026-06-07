/**
 * metar_display.ino
 * Weather display for CrowPanel ESP32 2.8" (ILI9341, 320x240).
 *
 * Required libraries (Arduino IDE Library Manager):
 *   - TFT_eSPI        v2.5.x  (configure with the User_Setup.h from CrowPanel docs)
 *   - TJpg_Decoder    latest  (by Bodmer)
 *   - ArduinoJson     v6.x    (by Benoit Blanchon)
 *   - NTPClient       latest  (by Fabrice Weinberg)
 *
 * Board: "ESP32 Dev Module" — esp32 by Espressif 2.0.14 / 2.0.15
 */

#include "config.h"

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <NTPClient.h>
#include <WiFiUDP.h>

// ── Display layout ────────────────────────────────────────────────────────────
#define SCREEN_W   320
#define SCREEN_H   240
#define BAR_H       20    // top status bar height

#define RADAR_X      0
#define RADAR_Y    BAR_H
#define RADAR_W    220
#define RADAR_H    (SCREEN_H - BAR_H)   // 220 px

#define INFO_X     RADAR_W              // 220
#define INFO_W     (SCREEN_W - RADAR_W) // 100

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define C_BAR_BG    0x000D   // very dark navy
#define C_DIVIDER   0x2945   // dark grey
#define C_LABEL     0x5DDF   // light cyan
#define C_SUBTLE    0x4208   // dim grey

// ── Globals ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

WiFiUDP    ntpUDP;
NTPClient  timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET_SEC);

// Buffer for JPEG download (~220×200 px, ~30-55 KB typical)
static uint8_t jpegBuf[62000];

// ── TJpgDec callback ──────────────────────────────────────────────────────────
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, 110);
    tft.print("Connecting to WiFi");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        tft.print(".");
    }
}

// ── HTTP download ─────────────────────────────────────────────────────────────
/**
 * Downloads URL into buf[0..bufSize-1].
 * Returns number of bytes read (0 on error).
 */
size_t downloadToBuffer(const char* url, uint8_t* buf, size_t bufSize) {
    WiFiClientSecure client;
    client.setInsecure();   // skip cert validation (acceptable for personal use)

    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Cache-Control", "no-cache");
    http.setTimeout(20000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP %d for %s\n", code, url);
        http.end();
        return 0;
    }

    int contentLen = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long deadline = millis() + 20000;

    if (contentLen > 0 && (size_t)contentLen <= bufSize) {
        // Known content length
        while (got < (size_t)contentLen && millis() < deadline) {
            int avail = stream->available();
            if (avail > 0) {
                int n = min(avail, (int)(contentLen - got));
                stream->readBytes(buf + got, n);
                got += n;
            } else {
                delay(1);
            }
        }
    } else {
        // Chunked / unknown length
        while ((stream->connected() || stream->available()) && got < bufSize && millis() < deadline) {
            if (stream->available()) {
                buf[got++] = stream->read();
            } else {
                delay(1);
            }
        }
    }

    http.end();
    return got;
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
void drawTopBar(const String& timeStr, const String& updatedAt) {
    tft.fillRect(0, 0, SCREEN_W, BAR_H, C_BAR_BG);
    tft.setTextSize(1);

    tft.setTextColor(TFT_WHITE, C_BAR_BG);
    tft.setCursor(4, 6);
    tft.print(timeStr);

    tft.setTextColor(C_DIVIDER, C_BAR_BG);
    tft.setCursor(144, 6);
    tft.print("upd ");
    tft.print(updatedAt);
}

void drawInfoPanel(float temp, int hum, int rain) {
    tft.fillRect(INFO_X, RADAR_Y, INFO_W, RADAR_H, TFT_BLACK);
    tft.drawFastVLine(INFO_X, RADAR_Y, RADAR_H, C_DIVIDER);

    int x = INFO_X + 6;

    // ── Temperature ───────────────────────────────────────
    tft.setTextSize(1);
    tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 8);
    tft.print("TEMP");

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 22);
    tft.printf("%.0f\xB0""C", temp);   // e.g. "27°C"

    // ── Humidity ──────────────────────────────────────────
    tft.drawFastHLine(INFO_X + 4, RADAR_Y + 56, INFO_W - 8, C_DIVIDER);

    tft.setTextSize(1);
    tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 62);
    tft.print("HUMID");

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 76);
    tft.printf("%d%%", hum);

    // ── Rain probability ──────────────────────────────────
    tft.drawFastHLine(INFO_X + 4, RADAR_Y + 110, INFO_W - 8, C_DIVIDER);

    tft.setTextSize(1);
    tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 116);
    tft.print("RAIN");
    tft.setCursor(x, RADAR_Y + 126);
    tft.print("6h");

    uint16_t rainColor = (rain > 60) ? TFT_RED : (rain > 30) ? TFT_YELLOW : TFT_GREEN;
    tft.setTextSize(2);
    tft.setTextColor(rainColor, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 140);
    tft.printf("%d%%", rain);

    // ── Station label ─────────────────────────────────────
    tft.setTextSize(1);
    tft.setTextColor(C_SUBTLE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 196);
    tft.print("MMGL");
}

void drawRadarPlaceholder() {
    tft.fillRect(RADAR_X, RADAR_Y, RADAR_W, RADAR_H, 0x000A);
    tft.setTextSize(1);
    tft.setTextColor(C_DIVIDER, 0x000A);
    tft.setCursor(55, RADAR_Y + 100);
    tft.print("Fetching radar...");
}

// ── Full update cycle ─────────────────────────────────────────────────────────
void updateDisplay() {
    Serial.println("Updating...");

    // 1. Fetch JSON (small payload, reuse buffer)
    float temp = 0; int hum = 0; int rain = 0; String upd = "--:--";
    size_t n = downloadToBuffer(DATA_URL, jpegBuf, sizeof(jpegBuf));
    if (n > 0) {
        DynamicJsonDocument doc(512);
        DeserializationError err = deserializeJson(doc, jpegBuf, n);
        if (!err) {
            temp = doc["temperature"] | 0.0f;
            hum  = doc["humidity"]    | 0;
            rain = doc["rain_probability"] | 0;
            upd  = doc["updated_at"].as<String>();
        } else {
            Serial.printf("JSON parse error: %s\n", err.c_str());
        }
    }

    // 2. Update time via NTP and draw top bar + info panel
    timeClient.update();
    String timeStr = timeClient.getFormattedTime().substring(0, 5) + " CST";
    drawTopBar(timeStr, upd);
    drawInfoPanel(temp, hum, rain);

    // 3. Fetch and draw radar JPEG
    drawRadarPlaceholder();
    n = downloadToBuffer(RADAR_URL, jpegBuf, sizeof(jpegBuf));
    if (n > 0) {
        Serial.printf("Radar: %u bytes\n", n);
        TJpgDec.drawJpg(RADAR_X, RADAR_Y, jpegBuf, n);
    } else {
        Serial.println("Radar fetch failed.");
    }

    Serial.printf("Update done. Temp=%.1f Hum=%d Rain=%d\n", temp, hum, rain);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Init display
    tft.begin();
    pinMode(27, OUTPUT);
    digitalWrite(27, HIGH);  // backlight on
    tft.setRotation(1);       // landscape
    tft.fillScreen(TFT_BLACK);

    // Init JPEG decoder
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);

    ensureWiFi();
    timeClient.begin();
    timeClient.update();

    updateDisplay();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static unsigned long lastUpdate    = 0;
static unsigned long lastTimeTick  = 0;

void loop() {
    ensureWiFi();

    unsigned long now = millis();

    // Full update every UPDATE_INTERVAL_MS (default 5 min)
    if (lastUpdate == 0 || now - lastUpdate >= UPDATE_INTERVAL_MS) {
        lastUpdate = now;
        updateDisplay();
        lastTimeTick = now;
        return;
    }

    // Update only the clock every minute (cheap: no HTTP requests)
    if (now - lastTimeTick >= 60000) {
        lastTimeTick = now;
        timeClient.update();
        String timeStr = timeClient.getFormattedTime().substring(0, 5) + " CST";
        // Redraw only the time portion of the top bar
        tft.fillRect(0, 0, 140, BAR_H, C_BAR_BG);
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE, C_BAR_BG);
        tft.setCursor(4, 6);
        tft.print(timeStr);
    }

    delay(100);
}
