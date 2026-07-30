# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Deterministic icon rendering for the Windows resource script.

Split out of build.py to keep it under BUILD_SCRIPT_SIZE_RATCHET, the same
one-way dependency every other tools/ module has: this module never imports
build.py.  build.py keeps the output paths and the staleness check; everything
about how a pixel gets its colour lives here.

The renderer is pure and deterministic -- same inputs, byte-identical .ico --
so a rebuild only rewrites an icon when build.py itself changed.
"""

import math
import struct
import zlib

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


def _luminance_grayscale(value):
    """Rec. 709 luminance of one style entry, preserving its type.

    Colours are (r, g, b) tuples of either ints (0..255 channel values) or
    floats (the additive bg/glow terms). Anything else -- a badge shape name,
    None -- passes through untouched, which is what keeps a derived style
    identical to its source apart from the colour.
    """
    if not isinstance(value, tuple) or len(value) != 3:
        return value
    r, g, b = value
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    if all(isinstance(channel, int) for channel in value):
        return (round(luma), round(luma), round(luma))
    return (luma, luma, luma)


def _grayscale_style(source_variant):
    """Derive a fully desaturated copy of an existing icon style.

    Generated rather than hand-written so the pending icon cannot drift away
    from the artwork it greys out: same geometry, same badge, same alpha, only
    the hue removed.
    """
    return {key: _luminance_grayscale(value)
            for key, value in ICON_STYLES[source_variant].items()}


# The transitional theme.  Shown from the moment a profile switch/Apply is
# initiated until the service reports the new settings are actually live -- a
# deliberately unhurried window (reset to a stock baseline, settle, write), so
# the icon must not keep advertising the old profile's colours nor jump to the
# new ones before the hardware has them.  Greyscale reads as "in transition"
# without inventing a fifth hue with its own meaning.
ICON_STYLES["tray_pending"] = _grayscale_style("tray_default")


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
