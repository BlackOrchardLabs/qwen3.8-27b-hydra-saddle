#!/usr/bin/env python3
"""Render an HTML artifact in headless Chrome and reject blank/broken pages."""

import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import zlib


def paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def png_metrics(path: pathlib.Path) -> dict:
    payload = path.read_bytes()
    if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("screenshot is not PNG")
    offset = 8
    width = height = bit_depth = color_type = None
    compressed = bytearray()
    while offset + 12 <= len(payload):
        length = struct.unpack(">I", payload[offset:offset + 4])[0]
        kind = payload[offset + 4:offset + 8]
        body = payload[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if not width or not height or bit_depth != 8 or color_type not in (0, 2, 4, 6):
        raise ValueError(f"unsupported PNG shape depth={bit_depth} color={color_type}")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    rows = []
    cursor = 0
    previous = bytearray(stride)
    for _ in range(height):
        filter_kind = raw[cursor]
        cursor += 1
        encoded = raw[cursor:cursor + stride]
        cursor += stride
        decoded = bytearray(stride)
        for index, byte in enumerate(encoded):
            left = decoded[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_kind == 0:
                value = byte
            elif filter_kind == 1:
                value = byte + left
            elif filter_kind == 2:
                value = byte + above
            elif filter_kind == 3:
                value = byte + ((left + above) // 2)
            elif filter_kind == 4:
                value = byte + paeth(left, above, upper_left)
            else:
                raise ValueError(f"unknown PNG filter {filter_kind}")
            decoded[index] = value & 0xFF
        rows.append(decoded)
        previous = decoded
    luminance = []
    step_y = max(1, height // 100)
    step_x = max(1, width // 100)
    for y in range(0, height, step_y):
        row = rows[y]
        for x in range(0, width, step_x):
            base = x * channels
            if color_type in (0, 4):
                value = row[base]
            else:
                red, green, blue = row[base:base + 3]
                value = (299 * red + 587 * green + 114 * blue) // 1000
            luminance.append(value)
    average = sum(luminance) / len(luminance)
    variance = sum((value - average) ** 2 for value in luminance) / len(luminance)
    return {
        "width": width,
        "height": height,
        "sample_count": len(luminance),
        "luma_min": min(luminance),
        "luma_max": max(luminance),
        "luma_variance": round(variance, 2),
        "distinct_luma": len(set(luminance)),
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(json.dumps({"ok": False, "error": "usage: html_render_gate.py ARTIFACT GATE_DIR"}))
        return 2
    artifact = pathlib.Path(sys.argv[1]).resolve()
    gate_dir = pathlib.Path(sys.argv[2]).resolve()
    gate_dir.mkdir(parents=True, exist_ok=True)
    html = artifact.read_bytes()
    lowered = html.lower()
    structural = {
        "doctype": b"<!doctype html" in lowered,
        "canvas": b"<canvas" in lowered,
        "script": b"<script" in lowered and b"</script>" in lowered,
        "minimum_bytes": len(html) >= 512,
    }
    chrome = shutil.which("google-chrome") or shutil.which("chromium") or shutil.which("chromium-browser")
    if chrome is None:
        print(json.dumps({"ok": False, "error": "headless Chrome executable not found", "structural": structural}))
        return 4
    screenshot = gate_dir / "screenshot.png"
    profile = gate_dir / "chrome-profile"
    cache = gate_dir / "chrome-cache"
    command = [
        chrome,
        "--headless=new",
        "--disable-gpu",
        "--disable-dev-shm-usage",
        "--disable-background-networking",
        "--enable-logging=stderr",
        "--v=0",
        f"--user-data-dir={profile}",
        f"--disk-cache-dir={cache}",
        "--window-size=1200,800",
        "--virtual-time-budget=3000",
        f"--screenshot={screenshot}",
        artifact.as_uri(),
    ]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=25)
    browser_log = completed.stdout[-16000:]
    console_errors = [
        line for line in browser_log.splitlines()
        if "CONSOLE" in line.upper() and ("ERROR" in line.upper() or "UNCAUGHT" in line.upper())
    ]
    metrics = png_metrics(screenshot) if screenshot.exists() else {}
    pixels_ok = bool(metrics) and metrics["distinct_luma"] >= 8 and metrics["luma_variance"] >= 20
    result = {
        "ok": completed.returncode == 0 and all(structural.values()) and not console_errors and pixels_ok,
        "artifact_sha256": hashlib.sha256(html).hexdigest(),
        "artifact_bytes": len(html),
        "chrome_exit": completed.returncode,
        "structural": structural,
        "console_errors": console_errors,
        "pixels_ok": pixels_ok,
        "pixel_metrics": metrics,
        "screenshot": str(screenshot),
        "screenshot_bytes": screenshot.stat().st_size if screenshot.exists() else 0,
        "browser_log_tail": browser_log[-3000:],
    }
    print(json.dumps(result, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

