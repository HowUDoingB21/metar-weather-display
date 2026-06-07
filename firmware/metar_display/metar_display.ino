/**
 * metar_display.ino
 * Weather display for CrowPanel ESP32 2.8" (ILI9341, 320x240).
 *
 * Required libraries (Arduino IDE Library Manager):
 *   - TFT_eSPI        v2.5.x  (copy User_Setup.h from CrowPanel docs)
 *   - TJpg_Decoder    latest  (by Bodmer)
 *   - ArduinoJson     v6.x    (by Benoit Blanchon)
 *   - NTPClient       latest  (by Fabrice Weinberg)
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
#define BAR_H       20

#define RADAR_X      0
#define RADAR_Y    BAR_H          // 20
#define RADAR_W    220
#define RADAR_H    (SCREEN_H - BAR_H)   // 220

#define INFO_X     RADAR_W        // 220
#define INFO_W     (SCREEN_W - RADAR_W) // 100

// Play button (pinned to bottom of right panel)
#define BTN_X      (INFO_X + 3)             // 223
#define BTN_H      34
#define BTN_Y      (SCREEN_H - BTN_H - 4)  // 202
#define BTN_W      (INFO_W - 6)             // 94

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define C_BAR_BG    0x000D
#define C_DIVIDER   0x2945
#define C_LABEL     0x5DDF
#define C_SUBTLE    0x4208
#define C_BTN_BG    0x0841   // dark navy (idle)
#define C_BTN_HL    0x1CFD   // bright sky-blue (pressed)
#define C_BTN_BDR   0x4A49

// ── Globals ───────────────────────────────────────────────────────────────────
TFT_eSPI   tft = TFT_eSPI();
WiFiUDP    ntpUDP;
NTPClient  timeClient(ntpUDP, "pool.ntp.org", NTP_OFFSET_SEC);

static uint8_t jpegBuf[62000];
static int     g_animCount  = 0;
static bool    g_animating  = false;

static unsigned long lastUpdate   = 0;
static unsigned long lastTimeTick = 0;

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
size_t downloadToBuffer(const char* url, uint8_t* buf, size_t bufSize) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Cache-Control", "no-cache");
    http.setTimeout(20000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP %d  %s\n", code, url);
        http.end();
        return 0;
    }

    int    contentLen = http.getSize();
    auto*  stream     = http.getStreamPtr();
    size_t got        = 0;
    unsigned long deadline = millis() + 20000;

    if (contentLen > 0 && (size_t)contentLen <= bufSize) {
        while (got < (size_t)contentLen && millis() < deadline) {
            int avail = stream->available();
            if (avail > 0) {
                got += stream->readBytes(buf + got,
                           min(avail, (int)(contentLen - got)));
            } else delay(1);
        }
    } else {
        while ((stream->connected() || stream->available())
               && got < bufSize && millis() < deadline) {
            if (stream->available()) buf[got++] = stream->read();
            else                     delay(1);
        }
    }
    http.end();
    return got;
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
void drawTopBar(const String& timeStr, const String& updAt) {
    tft.fillRect(0, 0, SCREEN_W, BAR_H, C_BAR_BG);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE,   C_BAR_BG);
    tft.setCursor(4, 6);
    tft.print(timeStr);
    tft.setTextColor(C_DIVIDER, C_BAR_BG);
    tft.setCursor(144, 6);
    tft.print("upd ");
    tft.print(updAt);
}

void drawPlayButton(bool highlight = false) {
    uint16_t bg = highlight ? C_BTN_HL : C_BTN_BG;
    tft.fillRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 5, bg);
    tft.drawRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 5, C_BTN_BDR);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setCursor(BTN_X + 10, BTN_Y + 6);
    tft.print("> 6H ANIM");
    tft.setCursor(BTN_X + 14, BTN_Y + 18);
    tft.setTextColor(0x7BEF, bg);   // lighter grey sub-label
    tft.print("radar motion");
}

void drawInfoPanel(float temp, int hum, int rain) {
    tft.fillRect(INFO_X, RADAR_Y, INFO_W, RADAR_H, TFT_BLACK);
    tft.drawFastVLine(INFO_X, RADAR_Y, RADAR_H, C_DIVIDER);

    int x = INFO_X + 6;

    // Temperature
    tft.setTextSize(1); tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 8);   tft.print("TEMP");
    tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 20);  tft.printf("%.0f\xB0""C", temp);

    tft.drawFastHLine(INFO_X + 4, RADAR_Y + 48, INFO_W - 8, C_DIVIDER);

    // Humidity
    tft.setTextSize(1); tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 54);  tft.print("HUMID");
    tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 66);  tft.printf("%d%%", hum);

    tft.drawFastHLine(INFO_X + 4, RADAR_Y + 98, INFO_W - 8, C_DIVIDER);

    // Rain probability
    tft.setTextSize(1); tft.setTextColor(C_LABEL, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 104); tft.print("RAIN");
    tft.setCursor(x, RADAR_Y + 115); tft.print("6h");
    uint16_t rc = (rain > 60) ? TFT_RED : (rain > 30) ? TFT_YELLOW : TFT_GREEN;
    tft.setTextSize(2); tft.setTextColor(rc, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 127); tft.printf("%d%%", rain);

    tft.drawFastHLine(INFO_X + 4, RADAR_Y + 155, INFO_W - 8, C_DIVIDER);

    // Station label (now has room, not overwritten by button)
    tft.setTextSize(1); tft.setTextColor(C_SUBTLE, TFT_BLACK);
    tft.setCursor(x, RADAR_Y + 162); tft.print("MMGL");

    // Play button pinned to bottom
    drawPlayButton();
}

void drawRadarPlaceholder(const char* msg = "Loading radar...") {
    tft.fillRect(RADAR_X, RADAR_Y, RADAR_W, RADAR_H, 0x000A);
    tft.setTextSize(1);
    tft.setTextColor(C_DIVIDER, 0x000A);
    tft.setCursor(55, RADAR_Y + 100);
    tft.print(msg);
}

// ── Full update ───────────────────────────────────────────────────────────────
void updateDisplay() {
    Serial.println("Updating...");

    // JSON (small, reuse buffer)
    char jsonBuf[512] = {};
    float temp = 0; int hum = 0; int rain = 0; String upd = "--:--";

    size_t n = downloadToBuffer(DATA_URL, (uint8_t*)jsonBuf, sizeof(jsonBuf) - 1);
    if (n > 0) {
        DynamicJsonDocument doc(512);
        if (!deserializeJson(doc, jsonBuf)) {
            temp        = doc["temperature"]      | 0.0f;
            hum         = doc["humidity"]         | 0;
            rain        = doc["rain_probability"] | 0;
            upd         = doc["updated_at"].as<String>();
            g_animCount = doc["anim_count"]       | 0;
        }
    }

    timeClient.update();
    drawTopBar(timeClient.getFormattedTime().substring(0, 5) + " CST", upd);
    drawInfoPanel(temp, hum, rain);

    drawRadarPlaceholder();
    n = downloadToBuffer(RADAR_URL, jpegBuf, sizeof(jpegBuf));
    if (n > 0) TJpgDec.drawJpg(RADAR_X, RADAR_Y, jpegBuf, n);

    Serial.printf("Done. T=%.1f H=%d R=%d Anim=%d\n", temp, hum, rain, g_animCount);
}

// ── Animation playback ────────────────────────────────────────────────────────
void playAnimation() {
    if (g_animCount == 0) return;
    g_animating = true;
    drawPlayButton(true);

    char url[100];
    for (int i = 0; i < g_animCount; i++) {
        // Small status label in top-bar (no full clear)
        tft.fillRect(0, 0, SCREEN_W, BAR_H, C_BAR_BG);
        tft.setTextColor(TFT_WHITE,  C_BAR_BG);
        tft.setTextSize(1);
        tft.setCursor(4, 6);
        tft.printf("Motion  %d / %d", i + 1, g_animCount);

        snprintf(url, sizeof(url),
                 "https://howudoingb21.github.io/metar-weather-display/anim_%02d.jpg", i);
        size_t n = downloadToBuffer(url, jpegBuf, sizeof(jpegBuf));
        if (n > 0) TJpgDec.drawJpg(RADAR_X, RADAR_Y, jpegBuf, n);

        // Brief pause so user can see each frame (download time counts toward it)
        delay(300);
    }

    // Show 6-hour forecast chart
    tft.fillRect(0, 0, SCREEN_W, BAR_H, C_BAR_BG);
    tft.setTextColor(TFT_YELLOW, C_BAR_BG);
    tft.setTextSize(1);
    tft.setCursor(4, 6);
    tft.print("6H FORECAST  (tap to close)");

    size_t n = downloadToBuffer(FORECAST_URL, jpegBuf, sizeof(jpegBuf));
    if (n > 0) TJpgDec.drawJpg(RADAR_X, RADAR_Y, jpegBuf, n);

    // Hold forecast until user taps or 8 s elapse
    unsigned long hold = millis();
    while (millis() - hold < 8000) {
        uint16_t tx, ty;
        if (tft.getTouch(&tx, &ty)) break;
        delay(80);
    }

    g_animating = false;
    updateDisplay();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    tft.begin();
    pinMode(27, OUTPUT);
    digitalWrite(27, HIGH);
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(tft_output);

    ensureWiFi();
    timeClient.begin();
    timeClient.update();

    updateDisplay();
    lastUpdate   = millis();
    lastTimeTick = lastUpdate;
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    if (!g_animating) ensureWiFi();

    unsigned long now = millis();

    // Periodic full update
    if (!g_animating && now - lastUpdate >= UPDATE_INTERVAL_MS) {
        lastUpdate   = now;
        lastTimeTick = now;
        updateDisplay();
        return;
    }

    // Clock tick (every minute, cheap)
    if (!g_animating && now - lastTimeTick >= 60000) {
        lastTimeTick = now;
        timeClient.update();
        String t = timeClient.getFormattedTime().substring(0, 5) + " CST";
        tft.fillRect(0, 0, 140, BAR_H, C_BAR_BG);
        tft.setTextColor(TFT_WHITE, C_BAR_BG);
        tft.setTextSize(1);
        tft.setCursor(4, 6);
        tft.print(t);
    }

    // Touch detection — only check play button
    if (!g_animating) {
        uint16_t tx, ty;
        if (tft.getTouch(&tx, &ty)) {
            if (tx >= BTN_X && tx <= BTN_X + BTN_W &&
                ty >= BTN_Y && ty <= BTN_Y + BTN_H) {
                drawPlayButton(true);  // instant highlight so user sees the press
                delay(150);            // hold highlight before download starts
                playAnimation();
            }
        }
    }

    delay(80);
}
