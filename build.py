#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Build script for Green Curve.

Downloads Zig if needed, generates the app icon, compiles resources,
then builds ``greencurve.exe``.
"""

import math
import os
import struct
import subprocess
import sys
import urllib.request
import zipfile
import zlib

ZIG_VERSION = "0.13.0"
ZIG_URL = (
    f"https://ziglang.org/download/{ZIG_VERSION}/zig-windows-x86_64-{ZIG_VERSION}.zip"
)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ZIG_DIR = os.path.join(SCRIPT_DIR, "zig")
ZIG_EXE = os.path.join(ZIG_DIR, "zig.exe")
SOURCE_DIR = os.path.join(SCRIPT_DIR, "source")
SOURCE_FILES = [
    os.path.join(SOURCE_DIR, "main.cpp"),
    os.path.join(SOURCE_DIR, "app_shared.cpp"),
    os.path.join(SOURCE_DIR, "config_utils.cpp"),
    os.path.join(SOURCE_DIR, "fan_curve.cpp"),
]
OUTPUT_EXE = os.path.join(SCRIPT_DIR, "greencurve.exe")
TEMP_OUTPUT_EXE = OUTPUT_EXE + ".new"
BACKUP_EXE = OUTPUT_EXE + ".bak"
ICON_ICO = os.path.join(SCRIPT_DIR, "greencurve.ico")
TRAY_ICON_DEFAULT_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_default.ico")
TRAY_ICON_OC_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_oc.ico")
TRAY_ICON_FAN_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_fan.ico")
TRAY_ICON_OC_FAN_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_oc_fan.ico")
ICON_RC = os.path.join(SCRIPT_DIR, "icon.rc")
ICON_RES = os.path.join(SCRIPT_DIR, "icon.res")

ICON_OUTPUTS = [
    ("app", ICON_ICO, (256, 128, 64, 48, 32, 24, 16)),
    ("tray_default", TRAY_ICON_DEFAULT_ICO, (64, 48, 32, 24, 16)),
    ("tray_oc", TRAY_ICON_OC_ICO, (64, 48, 32, 24, 16)),
    ("tray_fan", TRAY_ICON_FAN_ICO, (64, 48, 32, 24, 16)),
    ("tray_oc_fan", TRAY_ICON_OC_FAN_ICO, (64, 48, 32, 24, 16)),
]

COMMON_FLAGS = [
    "-std=c++17",
    "-Oz",
    "-DNDEBUG",
    "-fno-exceptions",
    "-fno-rtti",
    "-ffunction-sections",
    "-fdata-sections",
    f"-I{SOURCE_DIR}",
    "-Wl,--subsystem,windows",
    "-Wl,--gc-sections",
]

LINK_LIBS = [
    "-luser32",
    "-lgdi32",
    "-ladvapi32",
    "-lshell32",
]


def clamp01(value):
    if value < 0.0:
        return 0.0
    if value > 1.0:
        return 1.0
    return value


def clamp255(value):
    return max(0, min(255, int(value + 0.5)))


def lerp(a, b, t):
    return a + (b - a) * t


def lerp_color(a, b, t):
    return (
        lerp(a[0], b[0], t),
        lerp(a[1], b[1], t),
        lerp(a[2], b[2], t),
    )


def composite(dst, src):
    sr, sg, sb, sa = src
    dr, dg, db, da = dst
    src_a = sa / 255.0
    dst_a = da / 255.0
    out_a = src_a + dst_a * (1.0 - src_a)
    if out_a <= 0.0:
        return (0, 0, 0, 0)
    out_r = (sr * src_a + dr * dst_a * (1.0 - src_a)) / out_a
    out_g = (sg * src_a + dg * dst_a * (1.0 - src_a)) / out_a
    out_b = (sb * src_a + db * dst_a * (1.0 - src_a)) / out_a
    return (
        clamp255(out_r),
        clamp255(out_g),
        clamp255(out_b),
        clamp255(out_a * 255.0),
    )


def band_alpha(distance, target, half_width, feather):
    return clamp01((half_width + feather - abs(distance - target)) / feather)


def rounded_rect_distance(px, py, cx, cy, half_w, half_h, radius):
    qx = abs(px - cx) - half_w + radius
    qy = abs(py - cy) - half_h + radius
    ox = max(qx, 0.0)
    oy = max(qy, 0.0)
    outside = math.hypot(ox, oy)
    inside = min(max(qx, qy), 0.0)
    return outside + inside - radius


def point_segment_distance(px, py, ax, ay, bx, by):
    vx = bx - ax
    vy = by - ay
    wx = px - ax
    wy = py - ay
    vv = vx * vx + vy * vy
    if vv <= 1e-6:
        return math.hypot(wx, wy)
    t = (wx * vx + wy * vy) / vv
    t = clamp01(t)
    qx = ax + vx * t
    qy = ay + vy * t
    return math.hypot(px - qx, py - qy)


ICON_STYLES = {
    "app": {
        "bg_top": (8.0, 24.0, 18.0),
        "bg_bottom": (28.0, 76.0, 52.0),
        "glow": (18.0, 26.0, 10.0),
        "border": (108, 216, 164),
        "ring": (28, 178, 110),
        "arc": (198, 255, 226),
        "curve": (232, 255, 242),
        "node": (232, 255, 242),
        "badge": None,
    },
    "tray_default": {
        "bg_top": (8.0, 24.0, 18.0),
        "bg_bottom": (28.0, 76.0, 52.0),
        "glow": (18.0, 26.0, 10.0),
        "border": (108, 216, 164),
        "ring": (28, 178, 110),
        "arc": (198, 255, 226),
        "curve": (232, 255, 242),
        "node": (232, 255, 242),
        "badge": None,
    },
    "tray_oc": {
        "bg_top": (18.0, 18.0, 12.0),
        "bg_bottom": (72.0, 44.0, 18.0),
        "glow": (34.0, 18.0, 6.0),
        "border": (246, 184, 88),
        "ring": (228, 136, 42),
        "arc": (255, 233, 182),
        "curve": (255, 247, 228),
        "node": (255, 247, 228),
        "badge": "diamond",
        "badge_primary": (255, 176, 64),
        "badge_secondary": (255, 233, 182),
    },
    "tray_fan": {
        "bg_top": (8.0, 18.0, 24.0),
        "bg_bottom": (18.0, 54.0, 80.0),
        "glow": (10.0, 18.0, 34.0),
        "border": (110, 220, 240),
        "ring": (44, 176, 220),
        "arc": (210, 248, 255),
        "curve": (230, 251, 255),
        "node": (230, 251, 255),
        "badge": "circle",
        "badge_primary": (94, 226, 255),
        "badge_secondary": (210, 248, 255),
    },
    "tray_oc_fan": {
        "bg_top": (12.0, 18.0, 18.0),
        "bg_bottom": (44.0, 62.0, 54.0),
        "glow": (14.0, 24.0, 20.0),
        "border": (188, 228, 196),
        "ring": (132, 198, 168),
        "arc": (238, 255, 245),
        "curve": (245, 255, 250),
        "node": (245, 255, 250),
        "badge": "split",
        "badge_primary": (255, 176, 64),
        "badge_secondary": (94, 226, 255),
    },
}


def png_chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def rgba_to_png(rgba, size):
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)
        start = y * stride
        raw.extend(rgba[start : start + stride])
    compressed = zlib.compress(bytes(raw), 9)
    return b"".join(
        [
            b"\x89PNG\r\n\x1a\n",
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)),
            png_chunk(b"IDAT", compressed),
            png_chunk(b"IEND", b""),
        ]
    )


def render_icon(size, variant="app"):
    style = ICON_STYLES[variant]
    scale = size / 256.0
    center = size * 0.5
    margin = 18.0 * scale
    radius = 52.0 * scale
    ring_radius = 84.0 * scale
    ring_half_width = max(1.2, 8.0 * scale)
    curve_half_width = max(1.3, 6.0 * scale)
    node_radius = max(1.8, 7.0 * scale)
    border_width = max(1.2, 2.6 * scale)
    glow_x = 88.0 * scale
    glow_y = 76.0 * scale
    glow_r = 122.0 * scale
    shadow_cx = 128.0 * scale
    shadow_cy = 206.0 * scale
    shadow_rx = 86.0 * scale
    shadow_ry = 18.0 * scale
    badge_radius = 26.0 * scale
    badge_cx = size - margin - badge_radius * 0.85
    badge_cy = margin + badge_radius * 0.9
    curve_points = [
        (56.0 * scale, 162.0 * scale),
        (84.0 * scale, 154.0 * scale),
        (108.0 * scale, 138.0 * scale),
        (130.0 * scale, 122.0 * scale),
        (152.0 * scale, 108.0 * scale),
        (176.0 * scale, 100.0 * scale),
        (206.0 * scale, 100.0 * scale),
    ]
    pixels = bytearray(size * size * 4)

    for y in range(size):
        for x in range(size):
            px = x + 0.5
            py = y + 0.5
            dist_box = rounded_rect_distance(
                px,
                py,
                center,
                center,
                center - margin,
                center - margin,
                radius,
            )
            fill = clamp01((1.5 - dist_box) / 1.5)
            color = (0, 0, 0, 0)

            if fill > 0.0:
                t = py / max(1.0, size - 1.0)
                base_r, base_g, base_b = lerp_color(style["bg_top"], style["bg_bottom"], t)
                glow = clamp01(1.0 - math.hypot(px - glow_x, py - glow_y) / glow_r)
                base = (
                    clamp255(base_r + glow * style["glow"][0]),
                    clamp255(base_g + glow * style["glow"][1]),
                    clamp255(base_b + glow * style["glow"][2]),
                    clamp255(fill * 255.0),
                )
                color = composite(color, base)

                shadow = clamp01(
                    1.0
                    - (
                        ((px - shadow_cx) / shadow_rx) ** 2
                        + ((py - shadow_cy) / shadow_ry) ** 2
                    )
                )
                if shadow > 0.0:
                    color = composite(color, (0, 0, 0, clamp255(shadow * 38.0)))

                border = band_alpha(dist_box, 0.0, border_width, 1.0)
                if border > 0.0:
                    color = composite(color, (*style["border"], clamp255(border * 170.0)))

                dist_center = math.hypot(px - center, py - center)
                ring = band_alpha(dist_center, ring_radius, ring_half_width, 1.2)
                if ring > 0.0:
                    color = composite(color, (*style["ring"], clamp255(ring * 170.0)))

                angle = (
                    math.degrees(math.atan2(py - center, px - center)) + 360.0
                ) % 360.0
                if 210.0 <= angle <= 330.0:
                    arc = band_alpha(dist_center, ring_radius, ring_half_width, 1.0)
                    if arc > 0.0:
                        color = composite(color, (*style["arc"], clamp255(arc * 220.0)))

                min_curve_distance = 1e9
                for index in range(len(curve_points) - 1):
                    ax, ay = curve_points[index]
                    bx, by = curve_points[index + 1]
                    min_curve_distance = min(
                        min_curve_distance,
                        point_segment_distance(px, py, ax, ay, bx, by),
                    )
                curve = band_alpha(min_curve_distance, 0.0, curve_half_width, 1.0)
                if curve > 0.0:
                    color = composite(color, (*style["curve"], clamp255(curve * 255.0)))

                for node_x, node_y in curve_points:
                    node = band_alpha(
                        math.hypot(px - node_x, py - node_y), 0.0, node_radius, 1.0
                    )
                    if node > 0.0:
                        color = composite(color, (*style["node"], clamp255(node * 245.0)))

                badge_kind = style["badge"]
                if badge_kind == "diamond":
                    dist = abs(px - badge_cx) + abs(py - badge_cy)
                    badge = clamp01((badge_radius * 0.95 - dist) / max(1.0, 1.8 * scale))
                    if badge > 0.0:
                        color = composite(color, (*style["badge_primary"], clamp255(badge * 255.0)))
                        highlight = clamp01((badge_radius * 0.45 - (dist + (py - badge_cy) * 0.6)) / max(1.0, 1.2 * scale))
                        if highlight > 0.0:
                            color = composite(color, (*style["badge_secondary"], clamp255(highlight * 180.0)))
                elif badge_kind == "circle":
                    dist = math.hypot(px - badge_cx, py - badge_cy)
                    badge = band_alpha(dist, 0.0, badge_radius, max(1.0, 1.6 * scale))
                    if badge > 0.0:
                        color = composite(color, (*style["badge_primary"], clamp255(badge * 255.0)))
                        highlight = band_alpha(
                            math.hypot(px - (badge_cx - badge_radius * 0.28), py - (badge_cy - badge_radius * 0.28)),
                            0.0,
                            badge_radius * 0.42,
                            max(1.0, 1.2 * scale),
                        )
                        if highlight > 0.0:
                            color = composite(color, (*style["badge_secondary"], clamp255(highlight * 180.0)))
                elif badge_kind == "split":
                    dist = math.hypot(px - badge_cx, py - badge_cy)
                    badge = band_alpha(dist, 0.0, badge_radius, max(1.0, 1.6 * scale))
                    if badge > 0.0:
                        badge_color = style["badge_primary"] if px <= badge_cx else style["badge_secondary"]
                        color = composite(color, (*badge_color, clamp255(badge * 255.0)))
                        seam = band_alpha(abs(px - badge_cx), 0.0, max(1.0, 1.4 * scale), max(1.0, 1.0 * scale))
                        if seam > 0.0:
                            color = composite(color, (250, 250, 250, clamp255(seam * 120.0)))

            offset = (y * size + x) * 4
            pixels[offset + 0] = color[0]
            pixels[offset + 1] = color[1]
            pixels[offset + 2] = color[2]
            pixels[offset + 3] = color[3]

    return bytes(pixels)


def write_ico(path, variant, sizes):
    images = []
    for size in sizes:
        images.append((size, rgba_to_png(render_icon(size, variant), size)))

    header = struct.pack("<HHH", 0, 1, len(images))
    directory = bytearray()
    image_data = bytearray()
    offset = 6 + 16 * len(images)

    for size, png_bytes in images:
        directory.extend(
            struct.pack(
                "<BBBBHHII",
                0 if size >= 256 else size,
                0 if size >= 256 else size,
                0,
                0,
                1,
                32,
                len(png_bytes),
                offset,
            )
        )
        image_data.extend(png_bytes)
        offset += len(png_bytes)

    with open(path, "wb") as handle:
        handle.write(header)
        handle.write(directory)
        handle.write(image_data)


def generate_icon():
    """Generate the main app icon and tray-state icon variants."""
    for variant, path, sizes in ICON_OUTPUTS:
        write_ico(path, variant, sizes)


def compile_resources():
    """Compile the Windows resource file."""
    if os.path.exists(ICON_RES):
        os.remove(ICON_RES)

    cmd = [
        ZIG_EXE,
        "rc",
        "/x",
        f"/fo{ICON_RES}",
        ICON_RC,
    ]
    print(f"Compiling resources: {os.path.basename(ICON_RC)}")
    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0 or not os.path.exists(ICON_RES):
        print("Resource compilation FAILED")
        sys.exit(1)


def download_zig():
    """Download and extract Zig compiler."""
    if os.path.exists(ZIG_EXE):
        print(f"Zig already present at {ZIG_EXE}")
        return

    print(f"Downloading Zig {ZIG_VERSION}...")
    zip_path = os.path.join(SCRIPT_DIR, "zig.zip")

    try:
        urllib.request.urlretrieve(ZIG_URL, zip_path)
    except Exception as exc:
        print(f"Failed to download Zig: {exc}")
        print(f"Please download manually from: {ZIG_URL}")
        print(f"Extract to: {ZIG_DIR}")
        sys.exit(1)

    print("Extracting Zig...")
    os.makedirs(ZIG_DIR, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as archive:
        for member in archive.namelist():
            parts = member.split("/", 1)
            if len(parts) != 2 or not parts[1]:
                continue
            target = os.path.join(ZIG_DIR, parts[1])
            if member.endswith("/"):
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as handle:
                    handle.write(archive.read(member))

    os.remove(zip_path)

    if not os.path.exists(ZIG_EXE):
        print("ERROR: zig.exe not found after extraction")
        sys.exit(1)

    print(f"Zig installed at {ZIG_EXE}")


def compile_binary():
    """Compile the C++ sources using Zig's bundled clang."""
    missing_sources = [path for path in SOURCE_FILES if not os.path.exists(path)]
    if missing_sources:
        print("ERROR: Missing source files:")
        for path in missing_sources:
            print(f"  {path}")
        sys.exit(1)

    generate_icon()
    compile_resources()

    if os.path.exists(TEMP_OUTPUT_EXE):
        os.remove(TEMP_OUTPUT_EXE)

    cmd = [
        ZIG_EXE,
        "c++",
        *COMMON_FLAGS,
        "-o",
        TEMP_OUTPUT_EXE,
        *SOURCE_FILES,
        ICON_RES,
        *LINK_LIBS,
    ]

    print(f"Compiling {len(SOURCE_FILES)} source files -> {os.path.basename(OUTPUT_EXE)}")
    print(f"  Command: {' '.join(cmd)}")

    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0:
        if os.path.exists(TEMP_OUTPUT_EXE):
            os.remove(TEMP_OUTPUT_EXE)
        print("Compilation FAILED")
        sys.exit(1)

    if not os.path.exists(TEMP_OUTPUT_EXE):
        print(f"Compilation reported success but {TEMP_OUTPUT_EXE} is missing")
        sys.exit(1)

    if os.path.exists(BACKUP_EXE):
        os.remove(BACKUP_EXE)

    replaced_existing = False
    if os.path.exists(OUTPUT_EXE):
        try:
            os.replace(OUTPUT_EXE, BACKUP_EXE)
            replaced_existing = True
        except OSError as exc:
            print(f"WARNING: Could not replace existing output: {exc}")
            print(f"Built file kept at: {TEMP_OUTPUT_EXE}")
            print("Close any running greencurve.exe and rename the .new file manually.")
            sys.exit(1)

    try:
        os.replace(TEMP_OUTPUT_EXE, OUTPUT_EXE)
    except OSError as exc:
        if (
            replaced_existing
            and os.path.exists(BACKUP_EXE)
            and not os.path.exists(OUTPUT_EXE)
        ):
            os.replace(BACKUP_EXE, OUTPUT_EXE)
        print(f"Failed to finalize output: {exc}")
        print(f"Built file kept at: {TEMP_OUTPUT_EXE}")
        sys.exit(1)

    size = os.path.getsize(OUTPUT_EXE)
    print(f"Build successful: {OUTPUT_EXE} ({size:,} bytes / {size / 1024:.1f} KB)")


def main():
    print("=== Green Curve build ===")
    download_zig()
    compile_binary()
    print("=== Done ===")


if __name__ == "__main__":
    main()
