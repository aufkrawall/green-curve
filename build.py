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
SOURCE_FILE = os.path.join(SCRIPT_DIR, "main.cpp")
OUTPUT_EXE = os.path.join(SCRIPT_DIR, "greencurve.exe")
TEMP_OUTPUT_EXE = OUTPUT_EXE + ".new"
BACKUP_EXE = OUTPUT_EXE + ".bak"
ICON_ICO = os.path.join(SCRIPT_DIR, "greencurve.ico")
ICON_RC = os.path.join(SCRIPT_DIR, "icon.rc")
ICON_RES = os.path.join(SCRIPT_DIR, "icon.res")

COMMON_FLAGS = [
    "-std=c++17",
    "-Oz",
    "-DNDEBUG",
    "-fno-exceptions",
    "-fno-rtti",
    "-ffunction-sections",
    "-fdata-sections",
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


def render_icon(size):
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
                base_r = lerp(8.0, 28.0, t)
                base_g = lerp(24.0, 76.0, t)
                base_b = lerp(18.0, 52.0, t)
                glow = clamp01(1.0 - math.hypot(px - glow_x, py - glow_y) / glow_r)
                base = (
                    clamp255(base_r + glow * 18.0),
                    clamp255(base_g + glow * 26.0),
                    clamp255(base_b + glow * 10.0),
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
                    color = composite(color, (108, 216, 164, clamp255(border * 170.0)))

                dist_center = math.hypot(px - center, py - center)
                ring = band_alpha(dist_center, ring_radius, ring_half_width, 1.2)
                if ring > 0.0:
                    color = composite(color, (28, 178, 110, clamp255(ring * 170.0)))

                angle = (
                    math.degrees(math.atan2(py - center, px - center)) + 360.0
                ) % 360.0
                if 210.0 <= angle <= 330.0:
                    arc = band_alpha(dist_center, ring_radius, ring_half_width, 1.0)
                    if arc > 0.0:
                        color = composite(color, (198, 255, 226, clamp255(arc * 220.0)))

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
                    color = composite(color, (232, 255, 242, clamp255(curve * 255.0)))

                for node_x, node_y in curve_points:
                    node = band_alpha(
                        math.hypot(px - node_x, py - node_y), 0.0, node_radius, 1.0
                    )
                    if node > 0.0:
                        color = composite(
                            color, (232, 255, 242, clamp255(node * 245.0))
                        )

            offset = (y * size + x) * 4
            pixels[offset + 0] = color[0]
            pixels[offset + 1] = color[1]
            pixels[offset + 2] = color[2]
            pixels[offset + 3] = color[3]

    return bytes(pixels)


def generate_icon():
    """Generate a multi-size ICO file for the app."""
    images = []
    for size in (256, 128, 64, 48, 32, 24, 16):
        images.append((size, rgba_to_png(render_icon(size), size)))

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

    with open(ICON_ICO, "wb") as handle:
        handle.write(header)
        handle.write(directory)
        handle.write(image_data)


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
    """Compile main.cpp using Zig's bundled clang."""
    if not os.path.exists(SOURCE_FILE):
        print(f"ERROR: {SOURCE_FILE} not found")
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
        SOURCE_FILE,
        ICON_RES,
        *LINK_LIBS,
    ]

    print(
        f"Compiling {os.path.basename(SOURCE_FILE)} -> {os.path.basename(OUTPUT_EXE)}"
    )
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
