#!/usr/bin/env python3
"""
Weather data generator for ESP32 METAR display.
Runs via GitHub Actions every 10 minutes.
Outputs docs/data.json and docs/radar.jpg for the ESP32 to consume.
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
from PIL import Image, ImageDraw

# ── Configuration ──────────────────────────────────────────────────────────────
LAT          = 20.635617
LON          = -103.405235
TIMEZONE     = "America/Mexico_City"
RADAR_ZOOM   = 8
TILE_RADIUS  = 1        # 3×3 tile grid
OUTPUT_W     = 220      # radar image width (matches ESP32 panel)
OUTPUT_H     = 200      # radar image height
OUTPUT_DIR   = "docs"
BASEMAP_REFRESH_DAYS = 30

HEADERS = {
    "User-Agent": "metar-weather-display/1.0 (personal ESP32 weather station; github.com/HowUDoingB21/metar-weather-display)"
}

# ── Tile math ──────────────────────────────────────────────────────────────────
def lat_lon_to_tile(lat, lon, z):
    n = 2 ** z
    x = int((lon + 180) / 360 * n)
    lr = math.radians(lat)
    y = int((1 - math.log(math.tan(lr) + 1 / math.cos(lr)) / math.pi) / 2 * n)
    return x, y


def lat_lon_to_pixel_in_composite(lat, lon, tile_x0, tile_y0, z):
    """Pixel coords of (lat, lon) within a composite whose top-left tile is (tile_x0, tile_y0)."""
    n = 2 ** z
    gx = (lon + 180) / 360 * n * 256
    lr = math.radians(lat)
    gy = (1 - math.log(math.tan(lr) + 1 / math.cos(lr)) / math.pi) / 2 * n * 256
    return int(gx - tile_x0 * 256), int(gy - tile_y0 * 256)


# ── Tile fetchers ──────────────────────────────────────────────────────────────
def fetch_osm_tile(z, x, y):
    url = f"https://tile.openstreetmap.org/{z}/{x}/{y}.png"
    r = requests.get(url, headers=HEADERS, timeout=15)
    r.raise_for_status()
    return Image.open(BytesIO(r.content)).convert("RGBA")


def fetch_radar_tile(radar_path, z, x, y):
    # color=4 (Meteored palette), smooth=1, snow=0
    url = f"https://tilecache.rainviewer.com{radar_path}/256/{z}/{x}/{y}/4/1_0.png"
    r = requests.get(url, headers=HEADERS, timeout=15)
    if r.status_code == 200:
        return Image.open(BytesIO(r.content)).convert("RGBA")
    return None


def build_tile_composite(tile_fn, z, cx, cy, radius):
    """Assemble a (2r+1)×(2r+1) tile grid using tile_fn(z, x, y)."""
    size = (2 * radius + 1) * 256
    canvas = Image.new("RGBA", (size, size))
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            try:
                tile = tile_fn(z, cx + dx, cy + dy)
                if tile:
                    px = (dx + radius) * 256
                    py = (dy + radius) * 256
                    canvas.paste(tile, (px, py), mask=tile.split()[3])
                time.sleep(0.08)   # polite rate limiting
            except Exception as e:
                print(f"  tile ({cx+dx},{cy+dy}) failed: {e}", file=sys.stderr)
    return canvas


# ── API calls ──────────────────────────────────────────────────────────────────
def get_rainviewer_latest():
    r = requests.get(
        "https://api.rainviewer.com/public/weather-maps.json",
        headers=HEADERS, timeout=10
    )
    r.raise_for_status()
    data = r.json()
    frames = data.get("radar", {}).get("past", [])
    if not frames:
        return None, None
    latest = frames[-1]
    return latest["path"], latest["time"]


def fetch_open_meteo():
    url = (
        f"https://api.open-meteo.com/v1/forecast"
        f"?latitude={LAT}&longitude={LON}"
        f"&current=temperature_2m,relative_humidity_2m"
        f"&hourly=precipitation_probability"
        f"&timezone={TIMEZONE}&forecast_days=1"
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
        print("Fetching OSM base map tiles...")
        composite = build_tile_composite(fetch_osm_tile, RADAR_ZOOM, cx, cy, TILE_RADIUS)
        composite.save(basemap_path, "PNG")
        print(f"Base map saved ({os.path.getsize(basemap_path)//1024} KB)")
    else:
        composite = Image.open(basemap_path).convert("RGBA")
        print("Using cached base map.")
    return composite


def build_radar_image(basemap, radar_path, cx, cy):
    print("Fetching radar tiles...")
    radar_layer = build_tile_composite(
        lambda z, x, y: fetch_radar_tile(radar_path, z, x, y),
        RADAR_ZOOM, cx, cy, TILE_RADIUS
    )
    frame = basemap.copy()
    frame.alpha_composite(radar_layer)
    return frame


def crop_and_annotate(frame, cx, cy):
    tile_x0 = cx - TILE_RADIUS
    tile_y0 = cy - TILE_RADIUS
    home_x, home_y = lat_lon_to_pixel_in_composite(LAT, LON, tile_x0, tile_y0, RADAR_ZOOM)

    # Crop centered on home location
    x0 = max(0, home_x - OUTPUT_W // 2)
    y0 = max(0, home_y - OUTPUT_H // 2)
    x1 = min(frame.width,  x0 + OUTPUT_W)
    y1 = min(frame.height, y0 + OUTPUT_H)
    if x1 - x0 < OUTPUT_W:
        x0 = max(0, x1 - OUTPUT_W)
    if y1 - y0 < OUTPUT_H:
        y0 = max(0, y1 - OUTPUT_H)

    cropped = frame.crop((x0, y0, x1, y1))
    if cropped.size != (OUTPUT_W, OUTPUT_H):
        cropped = cropped.resize((OUTPUT_W, OUTPUT_H), Image.LANCZOS)

    # Draw home marker
    mx = home_x - x0
    my = home_y - y0
    draw = ImageDraw.Draw(cropped)
    r = 5
    draw.ellipse([mx-r, my-r, mx+r, my+r], fill=(255, 220, 0, 255), outline=(0, 0, 0, 255), width=2)
    draw.line([mx-r-3, my, mx+r+3, my], fill=(255, 220, 0, 200), width=1)
    draw.line([mx, my-r-3, mx, my+r+3], fill=(255, 220, 0, 200), width=1)

    return cropped


# ── Main ───────────────────────────────────────────────────────────────────────
def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    tz = pytz.timezone(TIMEZONE)
    now = datetime.now(tz)

    cx, cy = lat_lon_to_tile(LAT, LON, RADAR_ZOOM)
    print(f"Center tile ({cx},{cy}) zoom={RADAR_ZOOM}")

    # Base map (cached in repo, refreshed monthly)
    basemap_path = os.path.join(OUTPUT_DIR, "basemap.png")
    basemap = get_or_build_basemap(basemap_path, cx, cy)

    # Radar overlay
    radar_path, radar_ts = get_rainviewer_latest()
    if radar_path:
        frame = build_radar_image(basemap, radar_path, cx, cy)
    else:
        frame = basemap.copy()
        print("No radar data — using base map only.")
        radar_ts = None

    # Crop + home marker
    cropped = crop_and_annotate(frame, cx, cy)

    # Convert to RGB JPEG (dark background for transparent areas)
    rgb = Image.new("RGB", cropped.size, (15, 15, 25))
    rgb.paste(cropped, mask=cropped.split()[3])
    radar_out = os.path.join(OUTPUT_DIR, "radar.jpg")
    rgb.save(radar_out, "JPEG", quality=82, optimize=True)
    print(f"Radar saved ({os.path.getsize(radar_out)//1024} KB)")

    # Weather data
    print("Fetching weather + METAR...")
    weather = fetch_open_meteo()
    metar   = fetch_metar_mmgl()

    current = weather.get("current", {})
    hourly  = weather.get("hourly", {})
    probs   = hourly.get("precipitation_probability", [])
    rain_prob_6h = max(probs[:6]) if probs else 0

    data = {
        "temperature":     current.get("temperature_2m"),
        "humidity":        current.get("relative_humidity_2m"),
        "rain_probability": rain_prob_6h,
        "metar_wdir":      metar.get("wdir"),
        "metar_wspd":      metar.get("wspd"),
        "updated_at":      now.strftime("%H:%M"),
        "updated_epoch":   int(now.timestamp()),
        "radar_ts":        radar_ts,
    }

    data_out = os.path.join(OUTPUT_DIR, "data.json")
    with open(data_out, "w") as f:
        json.dump(data, f, indent=2)

    print(
        f"Done — Temp: {data['temperature']}°C  "
        f"Hum: {data['humidity']}%  "
        f"Rain(6h): {rain_prob_6h}%"
    )


if __name__ == "__main__":
    main()
