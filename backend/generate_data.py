#!/usr/bin/env python3
"""
Weather data generator for ESP32 METAR display.
Runs via GitHub Actions every 10 minutes.
Outputs radar.jpg, anim_00.jpg, forecast.jpg, and data.json under docs/.

Requires env var:
  OWM_API_KEY  — OpenWeatherMap API key (free at openweathermap.org)
"""
import math
import json
import os
import sys
import time
from io import BytesIO
from datetime import datetime

import requests
import pytz
from PIL import Image, ImageDraw, ImageFont

# ── Configuration ──────────────────────────────────────────────────────────────
LAT          = 20.635617
LON          = -103.405235
TIMEZONE     = "America/Mexico_City"

OWM_KEY      = os.environ.get("OWM_API_KEY", "")   # set in GitHub Actions secrets

RADAR_ZOOM   = 11       # zoom 11 → ~71 m/px; CROP covers ~12 km × 12 km
TILE_RADIUS  = 1        # 3×3 tile grid composite (768×768 px)

CROP_W       = 168      # px to crop before scaling — ≈12 km at zoom 11
CROP_H       = 168

OUTPUT_W     = 220      # final JPEG width  (matches ESP32 radar panel)
OUTPUT_H     = 220      # final JPEG height (fills full 220×220 radar panel)

OUTPUT_DIR   = "docs"
BASEMAP_REFRESH_DAYS = 30

HEADERS = {
    "User-Agent": (
        "metar-weather-display/1.0 "
        "(personal ESP32 weather station; "
        "github.com/HowUDoingB21/metar-weather-display)"
    )
}

# ── Tile math ──────────────────────────────────────────────────────────────────
def lat_lon_to_tile(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180) / 360 * n)
    lr = math.radians(lat)
    y = int((1 - math.log(math.tan(lr) + 1 / math.cos(lr)) / math.pi) / 2 * n)
    return x, y


def lat_lon_to_pixel_in_composite(lat, lon, tile_x0, tile_y0, z):
    n = 2 ** z
    gx = (lon + 180) / 360 * n * 256
    lr = math.radians(lat)
    gy = (1 - math.log(math.tan(lr) + 1 / math.cos(lr)) / math.pi) / 2 * n * 256
    return int(gx - tile_x0 * 256), int(gy - tile_y0 * 256)


# ── Tile fetchers ──────────────────────────────────────────────────────────────
def fetch_carto_tile(z, x, y):
    """CartoDB Positron (no labels) — light background, no road or city text."""
    url = f"https://a.basemaps.cartocdn.com/light_nolabels/{z}/{x}/{y}.png"
    r = requests.get(url, headers=HEADERS, timeout=15)
    r.raise_for_status()
    return Image.open(BytesIO(r.content)).convert("RGBA")


def fetch_owm_tile(z, x, y):
    """OpenWeatherMap precipitation tile — colored overlay, transparent where dry."""
    if not OWM_KEY:
        raise RuntimeError("OWM_API_KEY env variable not set")
    url = (f"https://tile.openweathermap.org/map/precipitation_new"
           f"/{z}/{x}/{y}.png?appid={OWM_KEY}")
    r = requests.get(url, headers=HEADERS, timeout=15)
    r.raise_for_status()
    return Image.open(BytesIO(r.content)).convert("RGBA")


def build_tile_composite(tile_fn, z, cx, cy, radius):
    size = (2 * radius + 1) * 256
    canvas = Image.new("RGBA", (size, size))
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            try:
                tile = tile_fn(z, cx + dx, cy + dy)
                if tile:
                    canvas.paste(tile, ((dx + radius) * 256, (dy + radius) * 256),
                                 mask=tile.split()[3])
            except Exception as e:
                print(f"  tile ({cx+dx},{cy+dy}): {e}", file=sys.stderr)
            time.sleep(0.05)
    return canvas


# ── Weather APIs ───────────────────────────────────────────────────────────────
def fetch_open_meteo():
    url = (
        f"https://api.open-meteo.com/v1/forecast"
        f"?latitude={LAT}&longitude={LON}"
        f"&current=temperature_2m,relative_humidity_2m"
        f"&hourly=precipitation_probability"
        f"&timezone={TIMEZONE}&forecast_days=2"
    )
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    return r.json()


def fetch_metar_mmgl():
    url = "https://aviationweather.gov/api/data/metar?ids=MMGL&format=json"
    r = requests.get(url, headers=HEADERS, timeout=10)
    r.raise_for_status()
    data = r.json()
    return data[0] if data else {}


# ── Image pipeline ─────────────────────────────────────────────────────────────
def get_or_build_basemap(basemap_path, cx, cy):
    age = float("inf")
    if os.path.exists(basemap_path):
        age = time.time() - os.path.getmtime(basemap_path)
    if age > BASEMAP_REFRESH_DAYS * 86400:
        print("Fetching CartoDB base map tiles (light, no labels)...")
        canvas = build_tile_composite(fetch_carto_tile, RADAR_ZOOM, cx, cy, TILE_RADIUS)
        canvas.save(basemap_path, "PNG")
        print(f"Base map saved ({os.path.getsize(basemap_path) // 1024} KB)")
    else:
        canvas = Image.open(basemap_path).convert("RGBA")
        print("Using cached base map.")
    return canvas


def composite_owm(basemap, cx, cy):
    """Overlay OWM precipitation tiles on basemap."""
    owm_layer = build_tile_composite(
        lambda z, x, y: fetch_owm_tile(z, x, y),
        RADAR_ZOOM, cx, cy, TILE_RADIUS
    )
    frame = basemap.copy()
    frame.alpha_composite(owm_layer)
    return frame


def crop_annotate_save(frame, cx, cy, out_path, label=None, quality=82):
    """Crop CROP_W×CROP_H centered on home, scale to OUTPUT_W×OUTPUT_H, save JPEG."""
    tile_x0 = cx - TILE_RADIUS
    tile_y0 = cy - TILE_RADIUS
    home_x, home_y = lat_lon_to_pixel_in_composite(LAT, LON, tile_x0, tile_y0, RADAR_ZOOM)

    x0 = max(0, home_x - CROP_W // 2)
    y0 = max(0, home_y - CROP_H // 2)
    x1 = min(frame.width,  x0 + CROP_W)
    y1 = min(frame.height, y0 + CROP_H)
    if x1 - x0 < CROP_W:
        x0 = max(0, x1 - CROP_W)
    if y1 - y0 < CROP_H:
        y0 = max(0, y1 - CROP_H)

    cropped = frame.crop((x0, y0, x1, y1))
    if cropped.size != (OUTPUT_W, OUTPUT_H):
        cropped = cropped.resize((OUTPUT_W, OUTPUT_H), Image.LANCZOS)

    # Home marker (yellow dot + crosshair)
    draw = ImageDraw.Draw(cropped)
    mx = int((home_x - x0) * OUTPUT_W / CROP_W)
    my = int((home_y - y0) * OUTPUT_H / CROP_H)
    r = 5
    draw.ellipse([mx-r, my-r, mx+r, my+r], fill=(255, 220, 0, 255), outline=(0, 0, 0, 255), width=2)
    draw.line([mx-r-3, my, mx+r+3, my], fill=(255, 220, 0, 200), width=1)
    draw.line([mx, my-r-3, mx, my+r+3], fill=(255, 220, 0, 200), width=1)

    # Timestamp label burned into image (bottom-left)
    if label:
        font = _load_font(10)
        lw = len(label) * 6 + 8
        draw.rectangle([(2, OUTPUT_H - 16), (2 + lw, OUTPUT_H - 2)], fill=(0, 0, 0, 200))
        draw.text((5, OUTPUT_H - 14), label, fill=(255, 230, 100), font=font)

    rgb = Image.new("RGB", cropped.size, (240, 240, 235))  # light background fill
    rgb.paste(cropped, mask=cropped.split()[3])
    rgb.save(out_path, "JPEG", quality=quality, optimize=True)
    return os.path.getsize(out_path)


# ── Font helper ────────────────────────────────────────────────────────────────
def _load_font(size=11):
    for fp in [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]:
        if os.path.exists(fp):
            try:
                return ImageFont.truetype(fp, size)
            except Exception:
                pass
    return ImageFont.load_default()


# ── Forecast chart ─────────────────────────────────────────────────────────────
def generate_forecast_image(hourly, cur_idx):
    """Build 6-bar forecast chart for hours H+1 … H+6 from cur_idx."""
    times = hourly.get("time", [])
    probs = hourly.get("precipitation_probability", [])

    forecast = []
    for k in range(1, 7):
        j = cur_idx + k
        if j < len(probs):
            try:
                h = datetime.strptime(times[j], "%Y-%m-%dT%H:%M").strftime("%H:00")
            except Exception:
                h = "--:--"
            forecast.append((h, probs[j] or 0))
        else:
            forecast.append(("--:--", 0))

    img  = Image.new("RGB", (OUTPUT_W, OUTPUT_H), (12, 12, 22))
    draw = ImageDraw.Draw(img)
    f_sm = _load_font(10)

    draw.text((8, 5), "6-HOUR RAIN FORECAST", fill=(110, 120, 200), font=f_sm)
    draw.line([(8, 19), (OUTPUT_W - 8, 19)], fill=(35, 35, 70), width=1)

    chart_left   = 8
    chart_bottom = 175
    chart_top    = 28
    chart_h      = chart_bottom - chart_top

    bar_area_w = OUTPUT_W - 16
    bar_w      = bar_area_w // 6

    for pct in [25, 50, 75]:
        gy = int(chart_bottom - pct / 100 * chart_h)
        draw.line([(chart_left, gy), (OUTPUT_W - 8, gy)], fill=(28, 28, 55), width=1)
        draw.text((OUTPUT_W - 26, gy - 5), f"{pct}%", fill=(40, 40, 80), font=f_sm)

    for i, (hlabel, prob) in enumerate(forecast):
        bx    = chart_left + i * bar_w
        bar_h = int(prob / 100 * chart_h)
        y_top = chart_bottom - bar_h

        color = (200, 55, 55) if prob > 60 else (195, 155, 45) if prob > 30 else (55, 175, 85)

        if bar_h > 0:
            draw.rectangle([bx + 2, y_top, bx + bar_w - 2, chart_bottom], fill=color)

        draw.text((bx + 2, max(y_top - 13, chart_top)), f"{prob}%",
                  fill=(215, 215, 215), font=f_sm)
        draw.text((bx + 1, chart_bottom + 4), hlabel, fill=(90, 100, 160), font=f_sm)

    draw.text((8, OUTPUT_H - 13), "Open-Meteo NWP  \xb7  tap anywhere to close",
              fill=(40, 45, 75), font=f_sm)

    out = os.path.join(OUTPUT_DIR, "forecast.jpg")
    img.save(out, "JPEG", quality=85)
    print("Forecast chart saved.")


# ── Main ───────────────────────────────────────────────────────────────────────
def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    tz  = pytz.timezone(TIMEZONE)
    now = datetime.now(tz)

    cx, cy = lat_lon_to_tile(LAT, LON, RADAR_ZOOM)
    print(f"Center tile ({cx},{cy}) zoom={RADAR_ZOOM}  (~12 km × 12 km view)")

    # ── Base map ──────────────────────────────────────────────────────────────
    basemap_path = os.path.join(OUTPUT_DIR, f"basemap_z{RADAR_ZOOM}.png")
    for fname in os.listdir(OUTPUT_DIR):
        if fname.startswith("basemap") and fname != os.path.basename(basemap_path):
            os.remove(os.path.join(OUTPUT_DIR, fname))
            print(f"Removed old basemap: {fname}")
    basemap = get_or_build_basemap(basemap_path, cx, cy)

    # ── Current radar frame (OWM precipitation overlay) ───────────────────────
    print("Building radar frame (OpenWeatherMap precipitation)...")
    current_frame = composite_owm(basemap, cx, cy)
    radar_out = os.path.join(OUTPUT_DIR, "radar.jpg")
    sz = crop_annotate_save(current_frame, cx, cy, radar_out)
    print(f"Radar saved ({sz // 1024} KB)")

    # ── Animation: single "now" frame + forecast chart ─────────────────────────
    for fname in os.listdir(OUTPUT_DIR):
        if fname.startswith("anim_") and fname.endswith(".jpg"):
            os.remove(os.path.join(OUTPUT_DIR, fname))

    anim_out = os.path.join(OUTPUT_DIR, "anim_00.jpg")
    sz = crop_annotate_save(current_frame, cx, cy, anim_out, label="now", quality=75)
    print(f"Animation frame (now): {sz // 1024} KB")
    anim_count = 1

    # ── Weather data ──────────────────────────────────────────────────────────
    print("Fetching weather + METAR...")
    weather = fetch_open_meteo()
    metar   = fetch_metar_mmgl()

    current = weather.get("current", {})
    hourly  = weather.get("hourly", {})
    times   = hourly.get("time", [])
    probs   = hourly.get("precipitation_probability", [])

    current_hour_str = now.strftime("%Y-%m-%dT%H:00")
    cur_idx = 0
    for i, t in enumerate(times):
        if t >= current_hour_str:
            cur_idx = i
            break

    rain_6h = max(
        (probs[cur_idx + k] for k in range(1, 7) if cur_idx + k < len(probs)),
        default=0
    )

    data = {
        "temperature":      current.get("temperature_2m"),
        "humidity":         current.get("relative_humidity_2m"),
        "rain_probability": rain_6h,
        "metar_wdir":       metar.get("wdir"),
        "metar_wspd":       metar.get("wspd"),
        "updated_at":       now.strftime("%H:%M"),
        "updated_epoch":    int(now.timestamp()),
        "anim_count":       anim_count,
    }
    with open(os.path.join(OUTPUT_DIR, "data.json"), "w") as f:
        json.dump(data, f, indent=2)

    # ── Forecast chart ────────────────────────────────────────────────────────
    generate_forecast_image(hourly, cur_idx)

    print(
        f"\nDone — Temp: {data['temperature']}°C  "
        f"Hum: {data['humidity']}%  Rain(6h): {rain_6h}%"
    )


if __name__ == "__main__":
    main()
