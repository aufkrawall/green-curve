#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Build script for Green Curve.

Downloads Zig if needed, generates the app icon, compiles resources,
then builds ``greencurve.exe``. Linux cross-builds use a separate source set.
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import tarfile
import time
import zipfile
import zlib

ZIG_VERSION = "0.13.0"

# Platform-dependent Zig download settings
if sys.platform == "win32":
    _ZIG_PLATFORM = "windows"
    _ZIG_ARCHIVE_EXT = ".zip"
    _ZIG_EXE_NAME = "zig.exe"
    _ZIG_SHA256 = "d859994725ef9402381e557c60bb57497215682e355204d754ee3df75ee3c158"
elif sys.platform.startswith("linux"):
    _ZIG_PLATFORM = "linux"
    _ZIG_ARCHIVE_EXT = ".tar.xz"
    _ZIG_EXE_NAME = "zig"
    _ZIG_SHA256 = "d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea"
else:
    print(f"Unsupported build host: {sys.platform}")
    sys.exit(1)

# GitHub release base for pre-packaged toolchain archives
COMPILERS_REPO_BASE = "https://github.com/aufkrawall/green-curve/releases/download/Compilers-1.0"
COMPILERS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compilers")

_ZIG_ARCHIVE_NAME = f"zig-{_ZIG_PLATFORM}-x86_64-{ZIG_VERSION}{_ZIG_ARCHIVE_EXT}"
ZIG_URL = f"{COMPILERS_REPO_BASE}/{_ZIG_ARCHIVE_NAME}"
ZIG_SHA256 = _ZIG_SHA256
ZIG_EXE_SHA256 = {
    "windows": "2e44af5bbf7a72ef8cbdae370284687c95d65a19affa469d2ad0364d905b8e84",
}.get(_ZIG_PLATFORM)

# llvm-mingw: portable MinGW toolchain for Windows builds with full CFG support
LLVM_MINGW_VERSION = "20260519"
LLVM_MINGW_ARCHIVE_NAME = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-x86_64.zip"
LLVM_MINGW_URL = f"{COMPILERS_REPO_BASE}/{LLVM_MINGW_ARCHIVE_NAME}"
LLVM_MINGW_SHA256 = "72dbd6e64614e3b3401998992d1bd9c8ace29e74611d71c80309ea71c3fb26f9"
LLVM_MINGW_CLANG_SHA256 = "e04c3380970bf64d07074c390f550371dbd12dbb46a263609b11cd164ac1faf8"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ZIG_DIR = os.path.join(SCRIPT_DIR, "zig")
ZIG_EXE = os.path.join(ZIG_DIR, _ZIG_EXE_NAME)
LLVM_MINGW_DIR = os.path.join(SCRIPT_DIR, "llvm-mingw")
LLVM_MINGW_CLANG = os.path.join(LLVM_MINGW_DIR, "bin", "clang++.exe")
LLVM_MINGW_RC = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-rc.exe")
SOURCE_DIR = os.path.join(SCRIPT_DIR, "source")
BUILD_WORK_DIR = os.path.join(SCRIPT_DIR, "build-tmp")
ZIG_GLOBAL_CACHE_DIR = os.path.join(BUILD_WORK_DIR, "zig-global-cache")
ZIG_LOCAL_CACHE_DIR = os.path.join(BUILD_WORK_DIR, "zig-local-cache")
BUILD_NUMBER_PATH = os.path.join(SCRIPT_DIR, "BUILD_NUMBER")
BUILD_FINGERPRINT_PATH = os.path.join(SCRIPT_DIR, ".build_fingerprint")
WINDOWS_SOURCE_FILES = [
    os.path.join(SOURCE_DIR, "main.cpp"),
    os.path.join(SOURCE_DIR, "app_shared.cpp"),
    os.path.join(SOURCE_DIR, "config_utils.cpp"),
    os.path.join(SOURCE_DIR, "fan_curve.cpp"),
    os.path.join(SOURCE_DIR, "ssp_glue.cpp"),
    os.path.join(SOURCE_DIR, "cfg_glue.cpp"),
    os.path.join(SOURCE_DIR, "service_acl.cpp"),
    os.path.join(SOURCE_DIR, "platform_win32.cpp"),
    os.path.join(SOURCE_DIR, "vf_backends.cpp"),
]


LINUX_SOURCE_FILES = [
    os.path.join(SOURCE_DIR, "linux_main.cpp"),
    os.path.join(SOURCE_DIR, "linux_port.cpp"),
    os.path.join(SOURCE_DIR, "linux_port_profiles.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_layout.cpp"),
    os.path.join(SOURCE_DIR, "linux_gpu.cpp"),
    os.path.join(SOURCE_DIR, "linux_backend.cpp"),
    os.path.join(SOURCE_DIR, "linux_daemon.cpp"),
    os.path.join(SOURCE_DIR, "platform_posix.cpp"),
    os.path.join(SOURCE_DIR, "vf_backends.cpp"),
]
WINDOWS_OUTPUT_EXE = os.path.join(SCRIPT_DIR, "greencurve.exe")
WINDOWS_TEMP_OUTPUT_EXE = WINDOWS_OUTPUT_EXE + ".new"
WINDOWS_BACKUP_EXE = WINDOWS_OUTPUT_EXE + ".bak"
WINDOWS_SERVICE_OUTPUT_EXE = os.path.join(SCRIPT_DIR, "greencurve-service.exe")
WINDOWS_SERVICE_TEMP_OUTPUT_EXE = WINDOWS_SERVICE_OUTPUT_EXE + ".new"
WINDOWS_SERVICE_BACKUP_EXE = WINDOWS_SERVICE_OUTPUT_EXE + ".bak"
# glibc-dynamic, NOT static-musl: the Linux backend dlopen()s the NVIDIA driver
# libraries (libnvidia-api.so.1 / libnvidia-ml.so.1), which are glibc shared
# objects.  A fully static musl binary cannot dlopen (musl's static dlopen is a
# failing stub), so the artifact must be dynamically linked against glibc.
LINUX_TARGET = "x86_64-linux-gnu"
LINUX_OUTPUT_BIN = os.path.join(SCRIPT_DIR, f"greencurve-{LINUX_TARGET}")
LINUX_TEMP_OUTPUT_BIN = LINUX_OUTPUT_BIN + ".new"
LINUX_BACKUP_BIN = LINUX_OUTPUT_BIN + ".bak"
ICON_ICO = os.path.join(SCRIPT_DIR, "greencurve.ico")
TRAY_ICON_DEFAULT_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_default.ico")
TRAY_ICON_OC_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_oc.ico")
TRAY_ICON_FAN_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_fan.ico")
TRAY_ICON_OC_FAN_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_oc_fan.ico")
ICON_RC = os.path.join(SCRIPT_DIR, "icon.rc")
ICON_RES = os.path.join(SCRIPT_DIR, "icon.res")

os.makedirs(ZIG_GLOBAL_CACHE_DIR, exist_ok=True)
os.makedirs(ZIG_LOCAL_CACHE_DIR, exist_ok=True)
os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", ZIG_GLOBAL_CACHE_DIR)
os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", ZIG_LOCAL_CACHE_DIR)

ICON_OUTPUTS = [
    ("app", ICON_ICO, (256, 128, 64, 48, 32, 24, 16)),
    ("tray_default", TRAY_ICON_DEFAULT_ICO, (64, 48, 32, 24, 16)),
    ("tray_oc", TRAY_ICON_OC_ICO, (64, 48, 32, 24, 16)),
    ("tray_fan", TRAY_ICON_FAN_ICO, (64, 48, 32, 24, 16)),
    ("tray_oc_fan", TRAY_ICON_OC_FAN_ICO, (64, 48, 32, 24, 16)),
]

ICON_RC_CONTENT = """// Generated by build.py. Do not edit by hand.
101 ICON "greencurve.ico"
111 ICON "greencurve_tray_default.ico"
112 ICON "greencurve_tray_oc.ico"
113 ICON "greencurve_tray_fan.ico"
114 ICON "greencurve_tray_oc_fan.ico"

1 VERSIONINFO
FILEVERSION     VER_MAJOR,VER_MINOR,VER_PATCH,VER_BUILD
PRODUCTVERSION  VER_MAJOR,VER_MINOR,VER_PATCH,VER_BUILD
FILEFLAGSMASK   0x3fL
#ifdef _DEBUG
FILEFLAGS       0x1L
#else
FILEFLAGS       0x0L
#endif
FILEOS          0x40004L
FILETYPE        0x1L
FILESUBTYPE     0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", "NVIDIA GPU VF Curve Editor"
            VALUE "FileVersion", "VER_STR"
            VALUE "InternalName", "GreenCurve"
            VALUE "LegalCopyright", "Copyright (c) 2026 aufkrawall. MIT License."
            VALUE "OriginalFilename", "greencurve.exe"
            VALUE "ProductName", "Green Curve"
            VALUE "ProductVersion", "VER_STR"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END

1 24 "greencurve.exe.manifest"
"""

COMMON_FLAGS = [
    "-std=c++17",
    "-Oz",
    "-DNDEBUG",
    "-fno-exceptions",
    "-fno-rtti",
    "-fstack-protector-strong",
    "-ffunction-sections",
    "-fdata-sections",
    f"-I{SOURCE_DIR}",
    "-Wl,--gc-sections",
    "-Wall",
    "-Wextra",
    "-Wshadow",
    "-Wformat=2",
    "-Wnull-dereference",
    "-Wundef",
    "-Wno-unused-function",
    "-Wno-unused-parameter",
    "-Werror",
    "-D_FORTIFY_SOURCE=2",
]

# C-002: read VERSION and inject it into all compile commands
APP_VERSION = "0.17.1"
APP_BUILD_NUMBER = 0
_version_path = os.path.join(SCRIPT_DIR, "VERSION")
if os.path.exists(_version_path):
    with open(_version_path, "r", encoding="utf-8") as _vf:
        APP_VERSION = _vf.read().strip()

SANITIZER_FLAGS = [
    "-fsanitize=undefined",
    "-fno-sanitize-recover=all",
    "-g",
]

WINDOWS_FLAGS = [
    # --allow-multiple-definition is REQUIRED because the MinGW CRT
    # unconditionally provides __guard_check_icall_fptr as a data pointer
    # in .00cfg (mingw_cfguard_support.o, pulled in by loadcfg.o's PE
    # load config references).  Our cfg_glue.cpp overrides it with a
    # proper function.  The duplicate is harmless — LLD uses our
    # definition (first on command line).  This is a known limitation
    # of MinGW's CFG implementation.
    "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va,--allow-multiple-definition",
    "-mguard=cf",
    "-fcf-protection=full",
    "-flto",
    "-Wl,--icf=safe",
    "-ftrivial-auto-var-init=pattern",
    "-fno-delete-null-pointer-checks",
    "-static",
    "-s",
]

LINUX_FLAGS = [
    "-target",
    LINUX_TARGET,
    # Dynamically linked against glibc so the backend can dlopen the NVIDIA
    # driver libraries at runtime (see LINUX_TARGET note).  Hardening kept.
    "-fPIE",
    "-pie",
    "-Wl,-z,relro,-z,now",
    "-Wl,-z,noexecstack",
    "-s",
    "-fstack-protector-strong",
    # Enable exceptions and RTTI for the Linux build, which uses <string> and
    # <vector> STL containers that depend on exception handling.  These
    # overrides come after the common -fno-exceptions -fno-rtti flags.
    "-fexceptions",
    "-frtti",
    # Runtime dynamic loading + threads for the backend/daemon.
    "-ldl",
    "-lpthread",
]

WINDOWS_LINK_LIBS = [
    "-luser32",
    "-lgdi32",
    "-ladvapi32",
    "-lshell32",
    "-lole32",
    "-lwtsapi32",
    "-luuid",
    "-ldbghelp",
    "-lversion",
    "-lcomctl32",
]

WINDOWS_SERVICE_LINK_LIBS = [
    "-lgdi32",
    "-ladvapi32",
    "-lshell32",
    "-lole32",
    "-lwtsapi32",
    "-luserenv",
    "-luuid",
    "-ldbghelp",
    "-lversion",
    "-lcomctl32",
]

# ---------------------------------------------------------------------------
# Multi-architecture build matrix
#
# Default `python build.py` builds Windows + Linux, each for x64 and arm64, and
# packages every (os, arch) into greencurve-<version>-<os>-<arch>.7z with all
# files under a greencurve/ subfolder.
# ---------------------------------------------------------------------------
WINDOWS_ARM64_TRIPLE = "aarch64-w64-mingw32"
LINUX_ARM64_TRIPLE = "aarch64-linux-gnu"


def linux_flags_for_arch(arch):
    """LINUX_FLAGS with the cross-compilation triple swapped for the arch."""
    flags = list(LINUX_FLAGS)
    if arch == "arm64":
        flags[flags.index("-target") + 1] = LINUX_ARM64_TRIPLE
        # Match the Windows arm64 build: -O2 over the common -Oz (avoids the
        # same arm64 size-opt codegen issue and keeps the two arches uniform),
        # plus BTI/PAC branch protection (the arm64 analogue of x86 CET).
        flags.append("-mbranch-protection=standard")
        flags.append("-O2")
    return flags


# Every (os, arch) build lands in its OWN isolated folder under dist/, using the
# canonical binary names (no -arch suffixes, no shared root or temp paths):
#   dist/<os>-<arch>/greencurve/{greencurve.exe, greencurve-service.exe | greencurve}
# That payload folder is also exactly what the 7z archives (a greencurve/ root).
DIST_DIR = os.path.join(SCRIPT_DIR, "dist")


def target_payload_dir(os_name, arch):
    """The isolated `greencurve/` payload folder for one (os, arch) target."""
    return os.path.join(DIST_DIR, f"{os_name}-{arch}", "greencurve")


def read_int_file(path, default=0):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read().strip()
        return int(text) if text else default
    except (OSError, ValueError):
        return default


def write_text_if_changed(path, text):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            if handle.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def iter_build_fingerprint_inputs():
    for name in ("build.py", "VERSION"):
        path = os.path.join(SCRIPT_DIR, name)
        if os.path.exists(path):
            yield path
    for root, _dirs, files in os.walk(SOURCE_DIR):
        for name in sorted(files):
            if name.endswith((".cpp", ".h")):
                yield os.path.join(root, name)


def compute_build_fingerprint():
    digest = hashlib.sha256()
    for path in sorted(iter_build_fingerprint_inputs()):
        rel = os.path.relpath(path, SCRIPT_DIR).replace("\\", "/")
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(65536), b""):
                digest.update(chunk)
        digest.update(b"\0")
    return digest.hexdigest()


def configure_build_number(bump_for_real_build):
    global APP_BUILD_NUMBER
    APP_BUILD_NUMBER = read_int_file(BUILD_NUMBER_PATH, 0)
    fingerprint = compute_build_fingerprint()
    previous = ""
    try:
        with open(BUILD_FINGERPRINT_PATH, "r", encoding="utf-8") as handle:
            previous = handle.read().strip()
    except OSError:
        previous = ""
    if bump_for_real_build and fingerprint != previous:
        APP_BUILD_NUMBER += 1
        write_text_if_changed(BUILD_NUMBER_PATH, f"{APP_BUILD_NUMBER}\n")
        write_text_if_changed(BUILD_FINGERPRINT_PATH, f"{fingerprint}\n")
        print(f"Build number bumped to {APP_BUILD_NUMBER}")
    elif bump_for_real_build and not os.path.exists(BUILD_FINGERPRINT_PATH):
        write_text_if_changed(BUILD_FINGERPRINT_PATH, f"{fingerprint}\n")
    COMMON_FLAGS.append(f"-DAPP_BUILD_NUMBER={APP_BUILD_NUMBER}")


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


def _any_newer(sources, target):
    """Return True if any source file is newer than the target, or if target is missing."""
    if not os.path.exists(target):
        return True
    target_mtime = os.path.getmtime(target)
    for src in sources:
        if os.path.exists(src) and os.path.getmtime(src) > target_mtime:
            return True
    return False


def generate_icon():
    """Generate the main app icon and tray-state icon variants if stale."""
    build_script = os.path.join(SCRIPT_DIR, "build.py")
    for variant, path, sizes in ICON_OUTPUTS:
        if _any_newer([build_script], path):
            write_ico(path, variant, sizes)


def _parse_version_parts():
    """Return (major, minor, patch, build) from APP_VERSION and APP_BUILD_NUMBER."""
    parts = APP_VERSION.split(".")
    major = int(parts[0]) if len(parts) > 0 else 0
    minor = int(parts[1]) if len(parts) > 1 else 0
    patch = int(parts[2]) if len(parts) > 2 else 0
    return major, minor, patch, APP_BUILD_NUMBER


APP_MANIFEST_CONTENT = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity
    type="win32"
    name="GreenCurve"
    version="VER_STR"
    processorArchitecture="amd64"/>
  <description>NVIDIA GPU VF Curve Editor</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v2">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <asmv3:application xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
    <asmv3:windowsSettings
      xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">
      <dpiAware>true</dpiAware>
    </asmv3:windowsSettings>
    <asmv3:windowsSettings
      xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>
    </asmv3:windowsSettings>
  </asmv3:application>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
      <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
      <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
      <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
    </application>
  </compatibility>
</assembly>
"""

MANIFEST_PATH = os.path.join(SCRIPT_DIR, "greencurve.exe.manifest")


def _build_rc_content():
    """Build RC content with version numbers substituted."""
    major, minor, patch, build = _parse_version_parts()
    ver_str = f"{major}.{minor}.{patch}.{build}"
    content = ICON_RC_CONTENT
    content = content.replace("VER_MAJOR", str(major))
    content = content.replace("VER_MINOR", str(minor))
    content = content.replace("VER_PATCH", str(patch))
    content = content.replace("VER_BUILD", str(build))
    content = content.replace("VER_STR", ver_str)
    return content


def _build_manifest_content():
    """Build manifest content with version substituted."""
    major, minor, patch, build = _parse_version_parts()
    ver_str = f"{major}.{minor}.{patch}.{build}"
    return APP_MANIFEST_CONTENT.replace("VER_STR", ver_str)


def generate_resource_script():
    """Generate the deterministic Windows resource script and manifest if missing or stale."""
    rc_content = _build_rc_content()
    manifest_content = _build_manifest_content()
    current_rc = None
    if os.path.exists(ICON_RC):
        with open(ICON_RC, "r", encoding="utf-8", errors="replace") as handle:
            current_rc = handle.read()
    if current_rc != rc_content:
        with open(ICON_RC, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(rc_content)
    current_manifest = None
    if os.path.exists(MANIFEST_PATH):
        with open(MANIFEST_PATH, "r", encoding="utf-8", errors="replace") as handle:
            current_manifest = handle.read()
    if current_manifest != manifest_content:
        with open(MANIFEST_PATH, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(manifest_content)


def compile_resources():
    """Compile the Windows resource file if stale using llvm-rc."""
    generate_resource_script()
    sources = [ICON_RC, MANIFEST_PATH] + [path for _, path, _ in ICON_OUTPUTS]
    if not _any_newer(sources, ICON_RES):
        return

    if os.path.exists(ICON_RES):
        os.remove(ICON_RES)

    cmd = [
        LLVM_MINGW_RC,
        "/x",
        f"/fo{ICON_RES}",
        ICON_RC,
    ]
    print(f"Compiling resources: {os.path.basename(ICON_RC)}")
    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0 or not os.path.exists(ICON_RES):
        print("Resource compilation FAILED")
        sys.exit(1)


def verify_sha256(path, expected):
    """Verify file SHA-256 matches expected value."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().lower() == expected.lower()


def safe_extract_target(base_dir, archive_name):
    """Resolve an archive member under base_dir and reject traversal."""
    relative = archive_name.replace("\\", "/")
    parts = relative.split("/", 1)
    if len(parts) != 2 or not parts[1]:
        return None
    relative = os.path.normpath(parts[1])
    if os.path.isabs(relative) or relative == ".." or relative.startswith(".." + os.sep):
        raise RuntimeError(f"Unsafe archive member: {archive_name}")
    base_abs = os.path.abspath(base_dir)
    target = os.path.abspath(os.path.join(base_abs, relative))
    if os.path.commonpath([base_abs, target]) != base_abs:
        raise RuntimeError(f"Unsafe archive member: {archive_name}")
    return target


def prepare_work_subdir(name):
    """Create a clean build scratch subdirectory inside the repository."""
    base_abs = os.path.abspath(BUILD_WORK_DIR)
    target = os.path.abspath(os.path.join(base_abs, name))
    if os.path.commonpath([base_abs, target]) != base_abs:
        raise RuntimeError(f"Unsafe build scratch path: {target}")
    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target, exist_ok=True)
    return target


def cleanup_work_subdir(path):
    if not path:
        return
    base_abs = os.path.abspath(BUILD_WORK_DIR)
    target = os.path.abspath(path)
    if os.path.commonpath([base_abs, target]) == base_abs and os.path.exists(target):
        shutil.rmtree(target, ignore_errors=True)


def _resolve_archive(source_label, archive_name, local_dir, url, sha256):
    """Return a (path, used_local) tuple for an archive.

    Checks *local_dir* first for *archive_name* (e.g. a vendored
    ``compilers/`` checkout).  Falls back to downloading from *url*.
    Always verifies the result against *sha256*.
    """
    local_path = os.path.join(local_dir, archive_name)
    temp_path = os.path.join(SCRIPT_DIR, archive_name)

    if os.path.exists(local_path):
        print(f"Using local {source_label} archive: {local_path}")
        shutil.copy2(local_path, temp_path)
        if sha256 and not verify_sha256(temp_path, sha256):
            os.remove(temp_path)
            print(f"ERROR: Local {source_label} archive failed SHA-256 verification")
            sys.exit(1)
        return temp_path

    print(f"Downloading {source_label} {archive_name}...")
    try:
        urllib.request.urlretrieve(url, temp_path)
    except Exception as exc:
        print(f"Failed to download {source_label}: {exc}")
        print(f"Please obtain from: {url}")
        sys.exit(1)

    if sha256 and not verify_sha256(temp_path, sha256):
        os.remove(temp_path)
        print(f"ERROR: {source_label} archive SHA-256 verification failed")
        sys.exit(1)
    return temp_path


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().lower()


def _write_integrity_sentinel(binary_path, trusted_sha256=None):
    sentinel = binary_path + ".sha256"
    digest = (trusted_sha256 or _sha256_file(binary_path)).lower()
    with open(sentinel, "w", encoding="utf-8") as f:
        f.write(digest)


def _verify_cached_tool_binary(binary_path, label, trusted_sha256):
    """Verify a cached tool binary against a digest pinned in this script.

    Adjacent ``.sha256`` files are not trusted: an attacker who can replace the
    binary can replace that sentinel too.  The sentinel is kept only as a
    post-extraction marker; the authority for cache reuse is the pinned digest.
    """
    if not trusted_sha256:
        print(f"WARNING: {label} has no pinned executable digest for this host; refreshing from pinned archive")
        return False
    try:
        current = _sha256_file(binary_path)
    except OSError as exc:
        print(f"WARNING: {label} cannot be read for integrity verification: {exc}")
        return False
    expected = trusted_sha256.lower()
    if current != expected:
        print(f"ERROR: {label} SHA-256 mismatch; cached binary will be refreshed")
        print(f"  expected (pinned): {expected}")
        print(f"  actual:            {current}")
        return False
    sentinel = binary_path + ".sha256"
    try:
        if os.path.exists(sentinel):
            with open(sentinel, "r", encoding="utf-8") as f:
                marker = f.read().strip().lower()
            if marker and marker != expected:
                print(f"WARNING: {label} sentinel differs from pinned digest; rewriting marker")
                _write_integrity_sentinel(binary_path, expected)
        else:
            _write_integrity_sentinel(binary_path, expected)
    except OSError as exc:
        print(f"WARNING: {label} could not update integrity sentinel: {exc}")
    return True


def download_zig():
    """Download (or copy from local compilers/) and extract Zig compiler."""
    if os.path.exists(ZIG_EXE) and _verify_cached_tool_binary(ZIG_EXE, "zig", ZIG_EXE_SHA256):
        print(f"Zig already present at {ZIG_EXE}")
        return

    zig_local_dir = os.path.join(COMPILERS_DIR, f"zig-{ZIG_VERSION}")
    archive_path = _resolve_archive(
        "Zig", _ZIG_ARCHIVE_NAME, zig_local_dir, ZIG_URL, ZIG_SHA256)

    print("Extracting Zig...")
    os.makedirs(ZIG_DIR, exist_ok=True)

    if _ZIG_ARCHIVE_EXT == ".zip":
        with zipfile.ZipFile(archive_path, "r") as archive:
            for member in archive.namelist():
                target = safe_extract_target(ZIG_DIR, member)
                if not target:
                    continue
                if member.endswith("/"):
                    os.makedirs(target, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with open(target, "wb") as handle:
                        handle.write(archive.read(member))
    else:
        with tarfile.open(archive_path, "r:xz") as archive:
            for member in archive.getmembers():
                target = safe_extract_target(ZIG_DIR, member.name)
                if not target or member.issym() or member.islnk():
                    continue
                if member.isdir():
                    os.makedirs(target, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    src = archive.extractfile(member)
                    if src:
                        with src, open(target, "wb") as handle:
                            handle.write(src.read())
                        if member.mode:
                            os.chmod(target, member.mode & 0o777)

    os.remove(archive_path)

    if not os.path.exists(ZIG_EXE):
        print(f"ERROR: {_ZIG_EXE_NAME} not found after extraction")
        sys.exit(1)

    print(f"Zig installed at {ZIG_EXE}")
    if ZIG_EXE_SHA256 and not _verify_cached_tool_binary(ZIG_EXE, "zig", ZIG_EXE_SHA256):
        print("ERROR: Extracted Zig binary failed pinned executable digest verification")
        sys.exit(1)
    _write_integrity_sentinel(ZIG_EXE, ZIG_EXE_SHA256)


def download_llvm_mingw():
    """Download (or copy from local compilers/) and extract llvm-mingw toolchain."""
    if os.path.exists(LLVM_MINGW_CLANG) and _verify_cached_tool_binary(LLVM_MINGW_CLANG, "llvm-mingw", LLVM_MINGW_CLANG_SHA256):
        print(f"llvm-mingw already present at {LLVM_MINGW_CLANG}")
        return

    llvm_local_dir = os.path.join(COMPILERS_DIR, f"llvm-mingw-{LLVM_MINGW_VERSION}")
    archive_path = _resolve_archive(
        "llvm-mingw", LLVM_MINGW_ARCHIVE_NAME, llvm_local_dir,
        LLVM_MINGW_URL, LLVM_MINGW_SHA256)

    print("Extracting llvm-mingw...")
    os.makedirs(LLVM_MINGW_DIR, exist_ok=True)

    with zipfile.ZipFile(archive_path, "r") as archive:
        for member in archive.namelist():
            target = safe_extract_target(LLVM_MINGW_DIR, member)
            if not target:
                continue
            if member.endswith("/"):
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as handle:
                    handle.write(archive.read(member))

    os.remove(archive_path)

    if not os.path.exists(LLVM_MINGW_CLANG):
        print("ERROR: clang++.exe not found after extraction")
        sys.exit(1)

    print(f"llvm-mingw installed at {LLVM_MINGW_DIR}")
    if not _verify_cached_tool_binary(LLVM_MINGW_CLANG, "llvm-mingw", LLVM_MINGW_CLANG_SHA256):
        print("ERROR: Extracted llvm-mingw binary failed pinned executable digest verification")
        sys.exit(1)
    _write_integrity_sentinel(LLVM_MINGW_CLANG, LLVM_MINGW_CLANG_SHA256)


def finalize_output(temp_output, output_path, backup_path=None, compile_started_at=None):
    if not os.path.exists(temp_output):
        if (compile_started_at is not None and os.path.exists(output_path) and
                os.path.getmtime(output_path) >= compile_started_at - 1.0):
            size = os.path.getsize(output_path)
            print(f"Build successful: {output_path} ({size:,} bytes / {size / 1024:.1f} KB; linker wrote final path directly)")
            return
        print(f"Compilation reported success but {temp_output} is missing")
        sys.exit(1)

    if backup_path and os.path.exists(backup_path):
        os.remove(backup_path)

    replaced_existing = False
    if os.path.exists(output_path):
        try:
            if backup_path:
                os.replace(output_path, backup_path)
            else:
                os.remove(output_path)
            replaced_existing = True
        except OSError as exc:
            print(f"WARNING: Could not replace existing output: {exc}")
            print(f"Built file kept at: {temp_output}")
            sys.exit(1)

    try:
        os.replace(temp_output, output_path)
    except OSError as exc:
        if backup_path and replaced_existing and os.path.exists(backup_path) and not os.path.exists(output_path):
            os.replace(backup_path, output_path)
        print(f"Failed to finalize output: {exc}")
        print(f"Built file kept at: {temp_output}")
        sys.exit(1)

    size = os.path.getsize(output_path)
    print(f"Build successful: {output_path} ({size:,} bytes / {size / 1024:.1f} KB)")


def get_windows_gui_compile_command(temp_output, arch="x64"):
    """Return the command array for compiling the Windows GUI executable.
    Uses llvm-mingw for x64 and Zig for arm64 (avoids llvm-mingw/LLD's
    aarch64 COFF "misaligned ldr/str offset" layout bug)."""
    if arch == "arm64":
        return [
            ZIG_EXE,
            "c++",
            *COMMON_FLAGS,
            "-target", "aarch64-windows-gnu",
            "-mbranch-protection=standard",
            "-flto",
            "-ftrivial-auto-var-init=pattern",
            "-fno-delete-null-pointer-checks",
            "-static",
            "-s",
            "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
            "-o",
            temp_output,
            *WINDOWS_SOURCE_FILES,
            ICON_RES,
            *WINDOWS_LINK_LIBS,
        ]
    return [
        LLVM_MINGW_CLANG,
        *COMMON_FLAGS,
        *WINDOWS_FLAGS,
        "-o",
        temp_output,
        *WINDOWS_SOURCE_FILES,
        ICON_RES,
        *WINDOWS_LINK_LIBS,
    ]


def get_windows_service_compile_command(temp_output, arch="x64"):
    """Return the command array for compiling the Windows service executable.
    Uses llvm-mingw for x64 and Zig for arm64 (avoids llvm-mingw/LLD's
    aarch64 COFF "misaligned ldr/str offset" layout bug)."""
    if arch == "arm64":
        return [
            ZIG_EXE,
            "c++",
            *COMMON_FLAGS,
            "-target", "aarch64-windows-gnu",
            "-mbranch-protection=standard",
            "-flto",
            "-ftrivial-auto-var-init=pattern",
            "-fno-delete-null-pointer-checks",
            "-static",
            "-s",
            "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
            "-DGREEN_CURVE_SERVICE_BINARY=1",
            "-o",
            temp_output,
            *WINDOWS_SOURCE_FILES,
            *WINDOWS_SERVICE_LINK_LIBS,
        ]
    return [
        LLVM_MINGW_CLANG,
        *COMMON_FLAGS,
        *WINDOWS_FLAGS,
        "-DGREEN_CURVE_SERVICE_BINARY=1",
        "-o",
        temp_output,
        *WINDOWS_SOURCE_FILES,
        *WINDOWS_SERVICE_LINK_LIBS,
    ]


def get_linux_compile_command(temp_output, arch="x64"):
    """Return the command array for compiling the Linux executable."""
    return [
        ZIG_EXE,
        "c++",
        *COMMON_FLAGS,
        *linux_flags_for_arch(arch),
        "-o",
        temp_output,
        *LINUX_SOURCE_FILES,
    ]


def compile_windows_binary(output_path=WINDOWS_OUTPUT_EXE, temp_output=WINDOWS_TEMP_OUTPUT_EXE, backup_path=WINDOWS_BACKUP_EXE, finalize=True, arch="x64"):
    """Compile the Windows GUI executable using Zig's bundled clang."""
    missing_sources = [path for path in WINDOWS_SOURCE_FILES if not os.path.exists(path)]
    if missing_sources:
        print("ERROR: Missing source files:")
        for path in missing_sources:
            print(f"  {path}")
        sys.exit(1)

    generate_icon()
    compile_resources()

    if os.path.exists(temp_output):
        os.remove(temp_output)

    cmd = get_windows_gui_compile_command(temp_output, arch)

    print(f"Compiling {len(WINDOWS_SOURCE_FILES)} source files -> {os.path.basename(output_path)} ({arch})")
    print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0:
        if os.path.exists(temp_output):
            os.remove(temp_output)
        print("Compilation FAILED")
        sys.exit(1)

    if finalize:
        finalize_output(temp_output, output_path, backup_path, compile_started_at)
    else:
        size = os.path.getsize(temp_output)
        print(f"Check build successful: {temp_output} ({size:,} bytes / {size / 1024:.1f} KB)")


def compile_windows_service_binary(output_path=WINDOWS_SERVICE_OUTPUT_EXE, temp_output=WINDOWS_SERVICE_TEMP_OUTPUT_EXE, backup_path=WINDOWS_SERVICE_BACKUP_EXE, finalize=True, arch="x64"):
    """Compile the dedicated Windows service executable."""
    missing_sources = [path for path in WINDOWS_SOURCE_FILES if not os.path.exists(path)]
    if missing_sources:
        print("ERROR: Missing source files:")
        for path in missing_sources:
            print(f"  {path}")
        sys.exit(1)

    if os.path.exists(temp_output):
        os.remove(temp_output)

    cmd = get_windows_service_compile_command(temp_output, arch)

    print(f"Compiling {len(WINDOWS_SOURCE_FILES)} source files -> {os.path.basename(output_path)} ({arch})")
    print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0:
        if os.path.exists(temp_output):
            os.remove(temp_output)
        print("Compilation FAILED")
        sys.exit(1)

    if finalize:
        finalize_output(temp_output, output_path, backup_path, compile_started_at)
    else:
        size = os.path.getsize(temp_output)
        print(f"Check build successful: {temp_output} ({size:,} bytes / {size / 1024:.1f} KB)")


def compile_linux_binary(output_path=LINUX_OUTPUT_BIN, temp_output=LINUX_TEMP_OUTPUT_BIN, backup_path=LINUX_BACKUP_BIN, finalize=True, arch="x64"):
    """Cross-compile the Linux glibc-dynamic binary (NvAPI/NVML dlopen)."""
    missing_sources = [path for path in LINUX_SOURCE_FILES if not os.path.exists(path)]
    if missing_sources:
        print("ERROR: Missing Linux source files:")
        for path in missing_sources:
            print(f"  {path}")
        sys.exit(1)

    if os.path.exists(temp_output):
        os.remove(temp_output)

    cmd = get_linux_compile_command(temp_output, arch)

    print(f"Compiling {len(LINUX_SOURCE_FILES)} source files -> {os.path.basename(output_path)} ({arch})")
    print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    if result.returncode != 0:
        if os.path.exists(temp_output):
            os.remove(temp_output)
        print("Compilation FAILED")
        sys.exit(1)

    if finalize:
        finalize_output(temp_output, output_path, backup_path, compile_started_at)
    else:
        size = os.path.getsize(temp_output)
        print(f"Check build successful: {temp_output} ({size:,} bytes / {size / 1024:.1f} KB)")


README_MD_PATH = os.path.join(SCRIPT_DIR, "README.md")
LICENSE_PATH = os.path.join(SCRIPT_DIR, "LICENSE")


def find_seven_zip():
    """Locate a 7-Zip executable (PATH or the standard Windows install dirs)."""
    on_path = shutil.which("7z") or shutil.which("7za") or shutil.which("7zr")
    if on_path:
        return on_path
    for cand in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if os.path.exists(cand):
            return cand
    return None


def detect_binary_arch(path):
    """Read the actual machine architecture from a PE (.exe) or ELF binary
    header.  Returns 'x64', 'arm64', or None if it can't be determined."""
    try:
        with open(path, "rb") as handle:
            head = handle.read(64)
            if head[:2] == b"MZ":  # PE / COFF (Windows)
                e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
                handle.seek(e_lfanew)
                if handle.read(4) != b"PE\x00\x00":
                    return None
                machine = struct.unpack("<H", handle.read(2))[0]
                return {0x8664: "x64", 0xAA64: "arm64"}.get(machine)
            if head[:4] == b"\x7fELF":  # ELF (Linux)
                machine = struct.unpack_from("<H", head, 18)[0]
                return {0x3E: "x64", 0xB7: "arm64"}.get(machine)
    except OSError:
        return None
    return None


def package_release_archive(os_name, arch, binaries):
    """Archive the target's isolated payload folder (dist/<os>-<arch>/greencurve)
    into greencurve-<version>-<os>-<arch>.7z.  The binaries were compiled directly
    into that folder; here we add README + LICENSE and zip it under a greencurve/
    root.

    Each binary's real PE/ELF machine field is verified to match `arch` before it
    is bundled, so a cross-arch mix (an arm64 GUI with an x64 service, etc.) is
    impossible — the build aborts on any mismatch."""
    payload = target_payload_dir(os_name, arch)
    for binary in binaries:
        if not os.path.exists(binary):
            print(f"WARNING: missing build output {binary}; skipping {os_name}-{arch} archive")
            return
        actual_arch = detect_binary_arch(binary)
        if actual_arch != arch:
            print(f"ERROR: architecture mismatch packaging {os_name}-{arch}: "
                  f"{os.path.basename(binary)} is {actual_arch or 'unrecognized'}, expected {arch}. "
                  f"Aborting to prevent a cross-arch bundle.")
            sys.exit(1)
    for extra in (README_MD_PATH, LICENSE_PATH):
        if os.path.exists(extra):
            shutil.copy2(extra, os.path.join(payload, os.path.basename(extra)))

    seven = find_seven_zip()
    if not seven:
        print(f"WARNING: 7-Zip not found (install from https://www.7-zip.org); "
              f"built {os_name}-{arch} files at {payload} but produced no archive")
        return
    archive = os.path.join(SCRIPT_DIR, f"greencurve-{APP_VERSION}-{os_name}-{arch}.7z")
    if os.path.exists(archive):
        os.remove(archive)
    # Run from the target dir and add the 'greencurve' folder so the archive root
    # holds greencurve/<files>.
    result = subprocess.run(
        [seven, "a", "-t7z", "-mx=9", "-bso0", "-bsp0", archive, "greencurve"],
        cwd=os.path.dirname(payload),
    )
    if result.returncode != 0 or not os.path.exists(archive):
        print(f"WARNING: 7-Zip archiving failed for {os_name}-{arch}")
        return
    size = os.path.getsize(archive)
    print(f"Archived {os.path.basename(archive)} ({size:,} bytes / {size / 1024:.1f} KB)")


def inspect_aarch64_driver(path):
    """Verify, WITHOUT arm64 hardware, that an aarch64 NVIDIA driver ships the
    libraries + symbols our backend needs.  `path` is an extracted
    NVIDIA-Linux-aarch64-<ver>.run directory (run `./<run> --extract-only` first)
    or a directory containing the .so files.  Reads aarch64 ELF exports with the
    bundled llvm-nm."""
    print(f"=== Inspecting aarch64 driver tree: {path} ===")
    if not os.path.isdir(path):
        print(f"ERROR: not a directory: {path}")
        print("Extract the driver first:  ./NVIDIA-Linux-aarch64-<ver>.run --extract-only")
        return 1

    wanted = {
        "NVML (libnvidia-ml.so)": (
            "libnvidia-ml.so",
            ["nvmlInit_v2", "nvmlDeviceGetCount_v2", "nvmlDeviceSetClockOffsets",
             "nvmlDeviceSetGpuLockedClocks", "nvmlDeviceGetGpcClkMinMaxVfOffset",
             "nvmlDeviceSetFanSpeed_v2", "nvmlDeviceSetPowerManagementLimit"]),
        "NvAPI (libnvidia-api.so)": (
            "libnvidia-api.so",
            ["nvapi_QueryInterface"]),
    }
    nm = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-nm.exe")
    if not os.path.exists(nm):
        nm = shutil.which("llvm-nm") or shutil.which("nm")

    found = {}   # label -> bool (library present AND all required symbols exported)
    for label, (prefix, symbols) in wanted.items():
        match = None
        for root, _dirs, files in os.walk(path):
            for fn in sorted(files):
                if fn.startswith(prefix) and ".so" in fn:
                    match = os.path.join(root, fn)
                    break
            if match:
                break
        if not match:
            print(f"  {label}: NOT FOUND")
            found[label] = False
            continue
        print(f"  {label}: {os.path.relpath(match, path)}")
        if not nm:
            print("    (no llvm-nm/nm available; cannot verify exported symbols)")
            found[label] = True  # present, symbols unverified
            continue
        try:
            exported = subprocess.run([nm, "-D", "--defined-only", match],
                                      capture_output=True, text=True, errors="ignore").stdout
        except OSError:
            print("    (symbol read failed)")
            found[label] = True
            continue
        all_syms = True
        for sym in symbols:
            present = sym in exported
            all_syms = all_syms and present
            print(f"      {'OK     ' if present else 'MISSING'} {sym}")
        found[label] = all_syms

    have_nvml = found.get("NVML (libnvidia-ml.so)", False)
    have_nvapi = found.get("NvAPI (libnvidia-api.so)", False)
    if not have_nvml:
        verdict = "NVML MISSING — this driver cannot drive the GPU (not a usable target)."
    elif have_nvapi:
        verdict = "FULL — NVML + NvAPI present: VF-curve OC/UV expected to work on this driver."
    else:
        verdict = ("NVML-ONLY — NvAPI absent: clock offsets / power / fan / locked clocks work, "
                   "but no VF-curve editing on this driver version.")
    print("\nVerdict:", verdict)
    return 0 if have_nvml else 1


def run_check_builds(target, generate_lsp=True):
    """Build selected targets into a temporary directory without replacing release outputs."""
    if generate_lsp:
        generate_lsp_files()
    tmp = prepare_work_subdir("check")
    try:
        if target in ("windows", "all"):
            compile_windows_binary(
                output_path=os.path.join(tmp, "greencurve.exe"),
                temp_output=os.path.join(tmp, "greencurve.exe.new"),
                backup_path="",
                finalize=False,
            )
            compile_windows_service_binary(
                output_path=os.path.join(tmp, "greencurve-service.exe"),
                temp_output=os.path.join(tmp, "greencurve-service.exe.new"),
                backup_path="",
                finalize=False,
            )
        if target in ("linux", "all"):
            compile_linux_binary(
                output_path=os.path.join(tmp, f"greencurve-{LINUX_TARGET}"),
                temp_output=os.path.join(tmp, f"greencurve-{LINUX_TARGET}.new"),
                backup_path="",
                finalize=False,
            )
    finally:
        cleanup_work_subdir(tmp)


def compile_flags_for_lsp(flags):
    result = []
    skip_next = False
    for index, flag in enumerate(flags):
        if skip_next:
            skip_next = False
            continue
        if flag in ("-o",):
            skip_next = True
            continue
        if flag in ("-static", "-s", "-pie"):
            continue
        if flag.startswith("-Wl,"):
            continue
        result.append(flag)
    return result


def generate_lsp_files():
    """Generate compile_commands.json for clangd from real build flags."""
    entries = []
    dummy_temp = os.path.join(SCRIPT_DIR, "dummy.out")
    gui_cmd = get_windows_gui_compile_command(dummy_temp)
    service_cmd = get_windows_service_compile_command(dummy_temp)
    linux_cmd = get_linux_compile_command(dummy_temp)
    # gui_cmd[0] = clang++; service_cmd[0] = clang++; linux_cmd[0] = zig
    # Skip the compiler executable (index 0), then strip trailing link args
    windows_flags = compile_flags_for_lsp(gui_cmd[1:-len(WINDOWS_SOURCE_FILES) - len(WINDOWS_LINK_LIBS) - 2])
    service_flags = compile_flags_for_lsp(service_cmd[1:-len(WINDOWS_SOURCE_FILES) - len(WINDOWS_SERVICE_LINK_LIBS) - 2])
    linux_flags = compile_flags_for_lsp(linux_cmd[2:-len(LINUX_SOURCE_FILES) - 2])

    entries.append({
        "directory": SCRIPT_DIR,
        "file": os.path.join(SOURCE_DIR, "main.cpp"),
        "arguments": ["clang++", *windows_flags, "-fsyntax-only", os.path.join(SOURCE_DIR, "main.cpp")],
    })
    entries.append({
        "directory": SCRIPT_DIR,
        "file": os.path.join(SOURCE_DIR, "main.cpp"),
        "arguments": ["clang++", *service_flags, "-fsyntax-only", os.path.join(SOURCE_DIR, "main.cpp")],
    })
    for source in LINUX_SOURCE_FILES:
        entries.append({
            "directory": SCRIPT_DIR,
            "file": source,
            "arguments": ["clang++", *linux_flags, "-fsyntax-only", source],
        })
    for source in (os.path.join(SOURCE_DIR, "app_shared.cpp"), os.path.join(SOURCE_DIR, "config_utils.cpp"), os.path.join(SOURCE_DIR, "fan_curve.cpp"), os.path.join(SOURCE_DIR, "service_acl.cpp")):
        entries.append({
            "directory": SCRIPT_DIR,
            "file": source,
            "arguments": ["clang++", *windows_flags, "-fsyntax-only", source],
        })

    path = os.path.join(SCRIPT_DIR, "compile_commands.json")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(entries, handle, indent=2)
        handle.write("\n")
    print(f"Generated {path}")


def run_build_script_regression_tests():
    tmp = prepare_work_subdir("build_script_regression")
    try:
        tool_path = os.path.join(tmp, "tool.bin")
        with open(tool_path, "wb") as handle:
            handle.write(b"trusted tool bytes")
        trusted = _sha256_file(tool_path)
        _write_integrity_sentinel(tool_path, trusted)
        if not _verify_cached_tool_binary(tool_path, "test-tool", trusted):
            print("Build-script regression FAILED: trusted cached tool rejected")
            sys.exit(1)
        with open(tool_path, "wb") as handle:
            handle.write(b"attacker replacement")
        _write_integrity_sentinel(tool_path, _sha256_file(tool_path))
        if _verify_cached_tool_binary(tool_path, "test-tool", trusted):
            print("Build-script regression FAILED: adjacent sentinel trusted over pinned digest")
            sys.exit(1)
    finally:
        cleanup_work_subdir(tmp)


def run_regression_tests(extra_flags=None):
    """Run pure regression tests that do not touch GPU hardware."""
    run_build_script_regression_tests()
    harness = r'''
#include "fan_curve.h"
#include "service_acl.h"
#include "vf_backends.h"
#include "linux_tui_layout.cpp"

bool is_curve_point_visible_in_gui(int) { return true; }
void debug_log(const char*, ...) {}
#include "config_profile_repair.cpp"

void invalidate_tray_profile_cache() {}

int main(int argc, char** argv) {
    InitializeCriticalSection(&g_configLock);

    if (APP_DEBUG_DEFAULT_ENABLED != 1) return 20;

    int point = -1;
    if (!parse_cli_point_arg_w(L"--point0", &point) || point != 0) return 21;
    if (!parse_cli_point_arg_w(L"--point127", &point) || point != 127) return 22;
    if (parse_cli_point_arg_w(L"--point128", &point)) return 23;
    if (parse_cli_point_arg_w(L"--pointabc", &point)) return 24;
    if (parse_cli_point_arg_w(L"--point-1", &point)) return 25;

    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_PASCAL)) return 26;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_TURING)) return 27;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_AMPERE)) return 28;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_LOVELACE)) return 29;
    if (gpu_family_uses_best_guess_backend(GPU_FAMILY_BLACKWELL)) return 30;
    if (!gpu_family_uses_best_guess_backend(GPU_FAMILY_UNKNOWN)) return 67;
    {
        GpuFamily fam = GPU_FAMILY_UNKNOWN;
        const VfBackendSpec* spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_GB200, &fam);
        if (!spec || fam != GPU_FAMILY_BLACKWELL || spec->bestGuessOnly) return 171;
        spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_AD100, &fam);
        if (!spec || fam != GPU_FAMILY_LOVELACE || spec->bestGuessOnly) return 172;
        spec = vf_backend_for_architecture(NV_GPU_ARCHITECTURE_GA100, &fam);
        if (!spec || fam != GPU_FAMILY_AMPERE || spec->bestGuessOnly) return 173;
        spec = vf_backend_for_architecture(0xDEADBEEFu, &fam);
        if (!spec || fam != GPU_FAMILY_UNKNOWN || !spec->bestGuessOnly) return 174;
    }

    FanCurveConfig cfg = {};
    fan_curve_set_default(&cfg);
    fan_curve_normalize(&cfg);
    char err[128] = {};
    if (!fan_curve_validate(&cfg, err, sizeof(err))) return 1;
    if (fan_curve_active_count(&cfg) != 5) return 2;
    if (fan_curve_interpolate_percent(&cfg, 30) != 20) return 3;
    int mid = fan_curve_interpolate_percent(&cfg, 52);
    if (mid < 42 || mid > 48) return 4;
    cfg.points[1].fanPercent = 10;
    if (fan_curve_validate(&cfg, err, sizeof(err))) return 5;
    cfg.points[1].fanPercent = 35;
    cfg.pollIntervalMs = 333;
    fan_curve_normalize(&cfg);
    if (cfg.pollIntervalMs != 250) return 6;

    FanCurveConfig invalidFanCurve = {};
    invalidFanCurve.pollIntervalMs = 333;
    invalidFanCurve.hysteresisC = 99;
    fan_curve_normalize(&invalidFanCurve);
    if (!fan_curve_validate(&invalidFanCurve, err, sizeof(err))) return 7;
    if (fan_curve_active_count(&invalidFanCurve) != 5) return 8;
    if (invalidFanCurve.pollIntervalMs != 250) return 9;
    if (invalidFanCurve.hysteresisC != FAN_CURVE_MAX_HYSTERESIS_C) return 10;
    FanCurveConfig onePointFanCurve = {};
    onePointFanCurve.pollIntervalMs = 500;
    onePointFanCurve.hysteresisC = 1;
    onePointFanCurve.points[7] = { gc_bool8_from_bool(true), 99, 100 };
    fan_curve_normalize(&onePointFanCurve);
    if (!fan_curve_validate(&onePointFanCurve, err, sizeof(err))) return 11;

    int parsedInt = 0;
    if (!parse_int_strict("2147483647", &parsedInt) || parsedInt != 2147483647) return 12;
    if (!parse_int_strict("-2147483648", &parsedInt) || parsedInt != (-2147483647 - 1)) return 13;
    if (parse_int_strict("999999999999999999999999", &parsedInt)) return 14;
    if (parse_int_strict("-999999999999999999999999", &parsedInt)) return 15;

    if (argc < 2 || !argv[1] || !argv[1][0]) return 31;
    DeleteFileA(argv[1]);
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return 32;
    leave_config_storage_lock(configMutex);
    if (get_config_int(argv[1], "debug", "enabled", 77) != 77) return 33;
    if (!set_config_int(argv[1], "debug", "enabled", APP_DEBUG_DEFAULT_ENABLED)) return 34;
    if (!config_section_has_keys(argv[1], "debug")) return 35;
    if (get_config_int(argv[1], "debug", "enabled", 0) != 1) return 36;
    if (!set_config_int(argv[1], "runtime", "selective_gpu_offset_mhz", 45)) return 37;
    if (get_config_int(argv[1], "runtime", "selective_gpu_offset_mhz", 0) != 45) return 38;
    // Lock mode (none/flatten/pin) must round-trip through the profile INI.
    // Pin (hard) loss on save was the user-reported bug; this guards the
    // serialize/parse contract the GUI relies on.
    if (!set_config_int(argv[1], "profile1", "lock_mode", LOCK_MODE_HARD)) return 39;
    if (get_config_int(argv[1], "profile1", "lock_mode", 0) != LOCK_MODE_HARD) return 46;
    if (!set_config_int(argv[1], "profile1", "lock_mode", LOCK_MODE_FLATTEN)) return 47;
    if (get_config_int(argv[1], "profile1", "lock_mode", 0) != LOCK_MODE_FLATTEN) return 48;
    DeleteFileA(argv[1]);

    // F-08-001: IPC object size and field layout sanity
    {
        if (sizeof(ServiceRequest) > 65535) return 70;
        if (sizeof(ServiceResponse) > 262143) return 71;
    }

    // F-08-001: validate_desired_settings_for_ipc extreme edge cases
    {
        DesiredSettings ds = {};
        validate_desired_settings_for_ipc(nullptr);
        validate_desired_settings_for_ipc(&ds);
        ds.hasPowerLimit = 7; ds.powerLimitPct = 0;
        ds.hasGpuOffset = 9; ds.gpuOffsetMHz = -50000;
        ds.hasMemOffset = 11; ds.memOffsetMHz = 99999;
        ds.hasFan = 13; ds.fanPercent = -100;
        ds.fanAuto = 15;
        ds.resetOcBeforeApply = 17;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            ds.hasCurvePoint[ci] = 19;
            ds.curvePointMHz[ci] = 9999999u;
        }
        ds.fanCurve.points[0].enabled = 21;
        validate_desired_settings_for_ipc(&ds);
        if (ds.hasPowerLimit != 1 || ds.hasGpuOffset != 1 || ds.hasMemOffset != 1) return 68;
        if (ds.hasFan != 1 || ds.fanAuto != 1 || ds.resetOcBeforeApply != 1) return 69;
        if (ds.hasCurvePoint[0] != 1 || ds.fanCurve.points[0].enabled != 1) return 79;
        if (ds.powerLimitPct != 50) return 72;
        if (ds.gpuOffsetMHz != -1000) return 73;
        if (ds.memOffsetMHz != 5000) return 74;
        if (ds.fanPercent != 0) return 75;
        if (ds.curvePointMHz[0] != 5000u) return 76;
        // Lock mode must clamp to the valid tri-state range at the IPC boundary.
        ds.lockMode = (LockMode)999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMode != LOCK_MODE_HARD) return 77;
        ds.lockMode = (LockMode)(-5);
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMode != LOCK_MODE_NONE) return 78;
    }

    // IPC response bool flags are canonicalized before the GUI trusts them.
    {
        ServiceResponse resp = {};
        resp.snapshot.initialized = 99;
        resp.snapshot.loaded = 88;
        resp.snapshot.selectedAdapterOrdinalFallback = 77;
        resp.snapshot.lastApplyUsedGpuOffset = 66;
        resp.snapshot.serviceInRecovery = 55;
        resp.snapshot.serviceReapplyInProgress = 44;
        resp.snapshot.adapterCount = 1;
        resp.snapshot.adapters[0].valid = 33;
        resp.snapshot.adapters[0].pciInfoValid = 22;
        resp.desired.hasGpuOffset = 11;
        resp.controlState.valid = 10;
        validate_service_response_for_ipc(&resp);
        if (resp.snapshot.initialized != 1 || resp.snapshot.loaded != 1) return 154;
        if (resp.snapshot.selectedAdapterOrdinalFallback != 1 || resp.snapshot.lastApplyUsedGpuOffset != 1) return 155;
        if (resp.snapshot.serviceInRecovery != 1 || resp.snapshot.serviceReapplyInProgress != 1) return 156;
        if (resp.snapshot.adapters[0].valid != 1 || resp.snapshot.adapters[0].pciInfoValid != 1) return 157;
        if (resp.desired.hasGpuOffset != 1 || resp.controlState.valid != 1) return 158;
    }

    // F-08-001: IPC magic/version constants unchanged
    {
        if (SERVICE_PROTOCOL_MAGIC != 0x47535643u) return 80;
        if (SERVICE_PROTOCOL_VERSION < 1) return 81;
    }

    // F-12-001: Backend spec VF_NUM_POINTS sanity
    {
        if (VF_NUM_POINTS != 128) return 90;
    }

    // F-15-002: degenerate/empty fan curve interpolation returns 100 (safe fallback)
    {
        FanCurveConfig empty = {};
        if (fan_curve_interpolate_percent(&empty, 50) != 100) return 40;
    }

    // fan_curve_clamp_percentages
    {
        FanCurveConfig clampCfg = {};
        fan_curve_set_default(&clampCfg);
        fan_curve_clamp_percentages(&clampCfg, 40, 80);
        for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
            if (clampCfg.points[i].enabled) {
                if (clampCfg.points[i].fanPercent < 40 || clampCfg.points[i].fanPercent > 80) return 41;
            }
        }
    }

    // fan_curve_equals
    {
        FanCurveConfig a = {}, b = {};
        fan_curve_set_default(&a);
        fan_curve_set_default(&b);
        if (!fan_curve_equals(&a, &b)) return 42;
        a.pollIntervalMs = 999;
        if (fan_curve_equals(&a, &b)) return 43;
    }

    // fan_curve_has_high_temp_low_fan_warning
    {
        FanCurveConfig safe = {};
        fan_curve_set_default(&safe);
        if (fan_curve_has_high_temp_low_fan_warning(&safe)) return 44;
        FanCurveConfig danger = {};
        danger.points[0] = { gc_bool8_from_bool(true), 85, 20 };
        danger.points[1] = { gc_bool8_from_bool(true), 95, 30 };
        if (!fan_curve_has_high_temp_low_fan_warning(&danger)) return 45;
    }

    // validate_desired_settings_for_ipc clamps out-of-range values
    {
        DesiredSettings ds = {};
        ds.hasPowerLimit = true;
        ds.powerLimitPct = 10;
        ds.hasGpuOffset = true;
        ds.gpuOffsetMHz = 5000;
        ds.hasMemOffset = true;
        ds.memOffsetMHz = -9000;
        ds.hasFan = true;
        ds.fanPercent = 200;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            ds.hasCurvePoint[ci] = true;
            ds.curvePointMHz[ci] = 9999u;
        }
        validate_desired_settings_for_ipc(&ds);
        if (ds.powerLimitPct != 50) return 50;
        if (ds.gpuOffsetMHz != 1000) return 51;
        if (ds.memOffsetMHz != -5000) return 52;
        if (ds.fanPercent != 100) return 53;
        if (ds.curvePointMHz[0] != 5000u) return 54;
    }

    // F-SEC-4: IPC validator also clamps index/mode/curve fields so a hostile
    // unprivileged caller cannot drive an out-of-bounds index, an unknown fan
    // mode, or out-of-range fan-curve values into the LocalSystem service.
    {
        DesiredSettings ds = {};
        ds.lockCi = 9999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockCi != VF_NUM_POINTS - 1) return 55;
        ds.lockCi = -42;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockCi != -1) return 56;
        ds.gpuOffsetExcludeLowCount = -5;
        validate_desired_settings_for_ipc(&ds);
        if (ds.gpuOffsetExcludeLowCount != 0) return 57;
        ds.gpuOffsetExcludeLowCount = 9999;
        validate_desired_settings_for_ipc(&ds);
        if (ds.gpuOffsetExcludeLowCount != VF_NUM_POINTS) return 58;
        ds.hasFan = true;
        ds.fanMode = 99;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanMode != FAN_MODE_CURVE) return 59;
        ds.fanMode = -7;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanMode != FAN_MODE_AUTO) return 60;
        ds.fanCurve.points[0].fanPercent = 250;
        ds.fanCurve.points[0].temperatureC = 9999;
        ds.fanCurve.points[1].fanPercent = -30;
        ds.fanCurve.points[1].temperatureC = -50;
        ds.fanCurve.hysteresisC = 999;
        ds.fanCurve.pollIntervalMs = 0;
        validate_desired_settings_for_ipc(&ds);
        if (ds.fanCurve.points[0].fanPercent != 100) return 61;
        if (ds.fanCurve.points[0].temperatureC != 150) return 62;
        if (ds.fanCurve.points[1].fanPercent != 0) return 63;
        if (ds.fanCurve.points[1].temperatureC != 0) return 64;
        if (ds.fanCurve.hysteresisC != FAN_CURVE_MAX_HYSTERESIS_C) return 65;
        if (ds.fanCurve.pollIntervalMs != 1) return 66;
    }

    // FP-08-003 RC6d: lock-serialization smoke test.  Proves the
    // g_serviceRuntimeLock mutex actually serializes two threads, which
    // is the invariant the RC6 fix relies on (recovery thread takes the
    // lock before closing NVML in Phase B, so the pipe-server APPLY
    // cannot race the close).  This test is a supplement to the
    // FP-08-003 source regression check (which is the deterministic
    // verification of the fix in the actual production code).  Without
    // serialization, the bug (recovery thread closes NVML under an
    // in-flight apply) would re-appear.
    {
        // Two synthetic threads against a fresh local CRITICAL_SECTION
        // (not g_serviceRuntimeLock, which is created lazily by
        // ensure_service_runtime_lock() and may be in an undefined
        // state during a hermetic test).  Each thread takes the CS,
        // sleeps 200 ms, then releases.  If the CS serializes, the
        // maximum concurrent-inside-CS count is exactly 1 and the
        // total wall-clock time from thread launch to thread completion
        // is at least ~400 ms (sum of both sleeps).  If the CS didn't
        // serialize, two threads could be inside simultaneously and
        // the total time would be ~200 ms (overlapping sleeps).
        struct Rc6dShared {
            CRITICAL_SECTION cs;
            volatile long count;
            long maxConcurrent;
        };
        static Rc6dShared s_rc6d = {};
        InitializeCriticalSection(&s_rc6d.cs);
        s_rc6d.count = 0;
        s_rc6d.maxConcurrent = 0;
        struct Rc6dThreadParams { Rc6dShared* s; };
        auto rc6dThreadProc = +[](void* p) -> DWORD {
            Rc6dThreadParams* params = (Rc6dThreadParams*)p;
            Rc6dShared* s = params->s;
            EnterCriticalSection(&s->cs);
            long incremented = (long)InterlockedIncrement(&s->count);
            if (incremented > s->maxConcurrent) {
                s->maxConcurrent = incremented;
            }
            Sleep(200);
            InterlockedDecrement(&s->count);
            LeaveCriticalSection(&s->cs);
            return 0;
        };
        Rc6dThreadParams params = {&s_rc6d};
        LARGE_INTEGER qpcStart, qpcEnd, qpcFreq;
        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&qpcStart);
        HANDLE t1 = CreateThread(nullptr, 0, rc6dThreadProc, &params, 0, nullptr);
        HANDLE t2 = CreateThread(nullptr, 0, rc6dThreadProc, &params, 0, nullptr);
        if (!t1 || !t2) {
            if (t1) CloseHandle(t1);
            if (t2) CloseHandle(t2);
            DeleteCriticalSection(&s_rc6d.cs);
            return 100;
        }
        HANDLE handles[2] = {t1, t2};
        WaitForMultipleObjects(2, handles, TRUE, 5000);
        QueryPerformanceCounter(&qpcEnd);
        CloseHandle(t1);
        CloseHandle(t2);
        long finalCount = s_rc6d.count;
        long maxConcurrent = s_rc6d.maxConcurrent;
        long long totalUs = ((qpcEnd.QuadPart - qpcStart.QuadPart) * 1000000LL) / qpcFreq.QuadPart;
        DeleteCriticalSection(&s_rc6d.cs);
        if (finalCount != 0) return 101;
        // Verify serialization: the maximum number of threads inside
        // the critical section simultaneously must be exactly 1
        // (otherwise the CS is broken).
        if (maxConcurrent != 1) return 102;
        // The total wall-clock time from thread launch to completion
        // must be at least 2 * 200 ms = 400 ms (the sum of both
        // sleeps).  If the CS didn't serialize, the second thread
        // would enter immediately after the first, and the total time
        // would be ~200 ms.  Allow 50 ms slop for tick resolution and
        // scheduler noise (use 350 ms threshold = 400 ms - 50 ms).
        if (totalUs < 350000LL) return 103;
    }

    // F-SEC-1: protected service-binary DACL round-trip.  apply_* must produce a
    // hardened DACL (inheritance disabled, no non-admin write); restore_* must
    // revert it so the file inherits its parent directory's ACLs again.  This
    // verifies the exact security property without needing a second identity.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 110;
        wchar_t aclFile[MAX_PATH] = {};
        StringCchPrintfW(aclFile, MAX_PATH, L"%lsgc_acl_%lu.bin", tempDir, GetCurrentProcessId());
        HANDLE ah = CreateFileW(aclFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ah == INVALID_HANDLE_VALUE) return 111;
        CloseHandle(ah);
        char aclErr[160] = {};
        // A fresh temp file inherits the (non-protected) temp dir ACL.
        if (service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 112; }
        if (!apply_protected_service_binary_dacl(aclFile, aclErr, sizeof(aclErr))) { DeleteFileW(aclFile); return 113; }
        if (!service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 114; }
        if (!restore_inherited_dacl(aclFile, aclErr, sizeof(aclErr))) { DeleteFileW(aclFile); return 115; }
        if (service_binary_dacl_is_hardened(aclFile)) { DeleteFileW(aclFile); return 116; }
        DeleteFileW(aclFile);
    }

    // F-SEC-1b: protected service install directory DACL.  The service binary
    // is installed adjacent to the GUI; the containing directory must also be
    // protected so a non-admin cannot delete/recreate the file by
    // parent-directory rights.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 166;
        wchar_t svcDir[MAX_PATH] = {};
        StringCchPrintfW(svcDir, MAX_PATH, L"%lsgc_service_dir_acl_%lu", tempDir, GetCurrentProcessId());
        if (!CreateDirectoryW(svcDir, nullptr)) return 167;
        char aclErr[256] = {};
        if (service_binary_dacl_is_hardened(svcDir)) { RemoveDirectoryW(svcDir); return 168; }
        if (!apply_protected_service_dir_dacl(svcDir, aclErr, sizeof(aclErr))) { RemoveDirectoryW(svcDir); return 169; }
        if (!service_binary_dacl_is_hardened(svcDir)) { RemoveDirectoryW(svcDir); return 170; }
        RemoveDirectoryW(svcDir);
    }

    // F-SEC-6: machine-wide config DACL round-trip.  The .ini must be readable
    // by standard users (so the GUI can show the current default) but writable
    // only by SYSTEM/Administrators.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 117;
        wchar_t mcFile[MAX_PATH] = {};
        StringCchPrintfW(mcFile, MAX_PATH, L"%lsgc_machine_acl_%lu.ini", tempDir, GetCurrentProcessId());
        HANDLE mh = CreateFileW(mcFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mh == INVALID_HANDLE_VALUE) return 118;
        CloseHandle(mh);
        char aclErr[256] = {};
        if (machine_config_dacl_is_hardened(mcFile)) { DeleteFileW(mcFile); return 119; }
        if (!apply_protected_machine_config_dacl(mcFile, aclErr, sizeof(aclErr))) { DeleteFileW(mcFile); return 120; }
        if (!machine_config_dacl_is_hardened(mcFile)) { DeleteFileW(mcFile); return 121; }
        DeleteFileW(mcFile);
    }

    // F-15: shared-bank DIRECTORY DACL round-trip.  %ProgramData%\Green Curve
    // must be standard-user-readable (list) but writable only by SYSTEM /
    // Administrators, so a non-admin cannot plant or delete shared bank files.
    {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return 130;
        wchar_t mcDir[MAX_PATH] = {};
        StringCchPrintfW(mcDir, MAX_PATH, L"%lsgc_machine_acl_dir_%lu", tempDir, GetCurrentProcessId());
        if (!CreateDirectoryW(mcDir, nullptr)) return 131;
        char aclErr[256] = {};
        if (machine_config_dacl_is_hardened(mcDir)) { RemoveDirectoryW(mcDir); return 132; }
        if (!apply_protected_machine_config_dir_dacl(mcDir, aclErr, sizeof(aclErr))) { RemoveDirectoryW(mcDir); return 133; }
        if (!machine_config_dacl_is_hardened(mcDir)) { RemoveDirectoryW(mcDir); return 134; }
        RemoveDirectoryW(mcDir);
    }

    // Shared-only policy: the "apply shared slot N" request flag must encode the
    // slot in bits 8..15 and the marker in bit 30, WITHOUT colliding with the
    // interactive bit (bit 0).  The service uses this to apply its own copy of an
    // admin shared profile for restricted callers.
    {
        DWORD f = SERVICE_REQUEST_FLAG_INTERACTIVE | SERVICE_REQUEST_FLAG_SHARED_SLOT |
                  ((3u & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
        if (!(f & SERVICE_REQUEST_FLAG_SHARED_SLOT)) return 135;
        if (!(f & SERVICE_REQUEST_FLAG_INTERACTIVE)) return 136;
        if (((f >> SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK) != 3u) return 137;
        DWORD f2 = SERVICE_REQUEST_FLAG_SHARED_SLOT | ((5u & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
        if ((f2 & SERVICE_REQUEST_FLAG_INTERACTIVE) != 0) return 138;
        if (((f2 >> SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK) != 5u) return 139;
    }

    // Pin-bug root-cause guard: the snapshot lockMode sync must NEVER adopt
    // the service's (previously applied) mode while the GUI holds divergent
    // pending lock intent (FLATTEN->HARD click / loaded HARD profile) or
    // unsaved edits.  Before this gate existed, the per-second telemetry
    // snapshot silently reverted a HARD pin to FLATTEN within ~1 s, making
    // the pin un-appliable ("No changes to apply") and saving it wrong.
    {
        // Divergent intent (clean): user clicked FLATTEN->HARD, snapshot still FLATTEN.
        if (lock_mode_sync_allowed(LOCK_MODE_HARD, LOCK_MODE_FLATTEN, false)) return 122;
        // Divergent intent (dirty): same, with other unsaved edits.
        if (lock_mode_sync_allowed(LOCK_MODE_HARD, LOCK_MODE_FLATTEN, true)) return 123;
        // No divergence but dirty: never resync mid-edit.
        if (lock_mode_sync_allowed(LOCK_MODE_FLATTEN, LOCK_MODE_FLATTEN, true)) return 124;
        // Clean, no divergence: adoption allowed (e.g. curve-detected FLATTEN
        // while the service authoritatively reports HARD at the same point).
        if (!lock_mode_sync_allowed(LOCK_MODE_FLATTEN, LOCK_MODE_FLATTEN, false)) return 125;
        if (!lock_mode_sync_allowed(LOCK_MODE_NONE, LOCK_MODE_NONE, false)) return 126;
    }

    // lockMHz is clamped at the IPC boundary like the curve points (it feeds
    // NVML locked-clocks and flatten-tail targets); 0 = "no target" stays 0.
    {
        DesiredSettings ds = {};
        ds.lockMHz = 4000000000u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 5000u) return 127;
        ds.lockMHz = 0u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 0u) return 128;
        ds.lockMHz = 2800u;
        validate_desired_settings_for_ipc(&ds);
        if (ds.lockMHz != 2800u) return 129;
    }

    // Logon auto-apply policy decision (resolve_logon_profile_source).  This is
    // the core of the restrict_non_admin_to_shared logon fix: a restricted user
    // (policy ON && not admin) must NEVER get their per-user custom OC applied at
    // logon — only an explicit published shared choice or the machine-wide shared
    // default.  Admins / unrestricted machines keep the legacy per-user-first
    // behavior.  The "restricted + per-user slot => NOT per-user" cases (142/143)
    // would have failed before the fix (bypass).
    {
        // Explicit, published shared choice always wins (any user).
        if (resolve_logon_profile_source(true, false, 3, true, true, true) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 140;
        if (resolve_logon_profile_source(false, true, 2, true, false, false) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 141;
        // Restricted + only a per-user slot => bypass closed (none, no default).
        if (resolve_logon_profile_source(true, false, 0, false, true, false) != LOGON_PROFILE_SOURCE_NONE) return 142;
        // Restricted + per-user slot + machine default => the shared default.
        if (resolve_logon_profile_source(true, false, 0, false, true, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 143;
        // Restricted + stale shared choice (unpublished) + default => default.
        if (resolve_logon_profile_source(true, false, 4, false, false, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 144;
        // Admin under policy keeps the per-user slot.
        if (resolve_logon_profile_source(true, true, 0, false, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 145;
        // Policy off, non-admin: per-user slot honored.
        if (resolve_logon_profile_source(false, false, 0, false, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 146;
        // Policy off, non-admin, no per-user but machine default => default.
        if (resolve_logon_profile_source(false, false, 0, false, false, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 147;
        // Nothing available => none (both unrestricted and restricted).
        if (resolve_logon_profile_source(false, false, 0, false, false, false) != LOGON_PROFILE_SOURCE_NONE) return 148;
        if (resolve_logon_profile_source(true, false, 0, false, false, false) != LOGON_PROFILE_SOURCE_NONE) return 149;
    }

    // Boot-time "reconcile the already-active session" safety net gating
    // (should_reconcile_active_session_at_boot).  This closes the Fast Startup /
    // autologon gap where the service comes up after the session is already
    // active and never sees a live WTS_SESSION_LOGON.  Args:
    //   (isManualStart, bootReconcileAlreadyDone, inRestartLoop,
    //    hasActiveInteractiveSession, alreadyAppliedThisSession)
    {
        // Boot auto-start, a user already logged in, nothing applied yet => apply.
        if (!should_reconcile_active_session_at_boot(false, false, false, true, false)) return 160;
        // Manual (--manual) GUI/CLI start must stay non-mutating, even with an
        // active session that has not been applied for.
        if (should_reconcile_active_session_at_boot(true, false, false, true, false)) return 161;
        // Already reconciled this boot (SCM crash-restart within the same boot).
        if (should_reconcile_active_session_at_boot(false, true, false, true, false)) return 162;
        // Restart-loop breaker tripped => do not add another apply this round.
        if (should_reconcile_active_session_at_boot(false, false, true, true, false)) return 163;
        // No one logged in yet => nothing to reconcile (a later real logon drives it).
        if (should_reconcile_active_session_at_boot(false, false, false, false, false)) return 164;
        // The live logon router already applied for this identity => no double-apply.
        if (should_reconcile_active_session_at_boot(false, false, false, true, true)) return 165;
        // Manual start dominates every other gate (defense in depth).
        if (should_reconcile_active_session_at_boot(true, false, false, true, true)) return 166;
        if (should_reconcile_active_session_at_boot(true, true, true, true, true)) return 167;
    }

    // logon_shared_slot round-trips through the profile INI like the other
    // [profiles] keys (the save/clear rewriters must re-emit it; guarded by the
    // source regression checks for the actual rewrite paths).
    {
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 3)) return 150;
        if (get_config_int(argv[1], "profiles", "logon_shared_slot", 0) != 3) return 151;
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 0)) return 152;
        if (get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 0) return 153;
        DeleteFileA(argv[1]);
    }

    // Profile repair must tolerate INT_MIN offsets without signed overflow.
    // Under UBSan the old abs(INT_MIN) implementation aborted in this case.
    {
        DesiredSettings repair = {};
        repair.hasLock = true;
        repair.lockCi = 10;
        repair.lockMHz = 2000;
        repair.hasCurvePoint[7] = true;
        repair.curvePointMHz[7] = 1700;
        repair.hasCurvePoint[8] = true;
        repair.curvePointMHz[8] = 1900;
        repair.hasCurvePoint[9] = true;
        repair.curvePointMHz[9] = 1950;
        for (int ci = 10; ci < 14; ci++) {
            repair.hasCurvePoint[ci] = true;
            repair.curvePointMHz[ci] = 2000;
        }
        DeleteFileA(argv[1]);
        if (!set_config_int(argv[1], "curve", "point7_offset_khz", (-2147483647 - 1))) return 159;
        repair_profile_locked_curve_readback_artifacts(argv[1], "curve", 1, &repair);
        if (!repair.hasCurvePoint[7]) return 160;
        DeleteFileA(argv[1]);
    }

    // CommandLineToArgvW-compatible quoting for elevated helper argv.
#if defined(_WIN32)
    {
        WCHAR cmd[512] = {};
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"--config")) return 161;
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"C:\\Path With Spaces\\quote\\\"tail\\\\config.ini")) return 162;
        if (!pl_append_quoted_arg_w(cmd, ARRAY_COUNT(cmd), L"--flag")) return 163;
        int parsedArgc = 0;
        LPWSTR* parsed = CommandLineToArgvW(cmd, &parsedArgc);
        if (!parsed) return 164;
        bool quoteOk = parsedArgc == 3 &&
            wcscmp(parsed[0], L"--config") == 0 &&
            wcscmp(parsed[1], L"C:\\Path With Spaces\\quote\\\"tail\\\\config.ini") == 0 &&
            wcscmp(parsed[2], L"--flag") == 0;
        LocalFree(parsed);
        if (!quoteOk) return 165;
    }
#endif

    // F-LNX-TUI: the Linux TUI button hitboxes are derived from the same line
    // list the renderer prints, so a click/focus rectangle can never drift off
    // the drawn "[label]".  This is the regression guard for the reported "mouse
    // offset grows the further down you go" bug (a hand-tracked row counter that
    // fell out of sync with the printed rows) and the byte-vs-display-column X
    // offset on lines that contain the multibyte degC glyph.
    {
        DesiredSettings tuiDesired = {};
        tuiDesired.fanCurve.points[0].temperatureC = 40;  // exercise the degC column
        TuiViewModel vm = {};
        vm.desired = &tuiDesired;
        vm.currentSlot = 1;
        vm.vfPage = 0;
        vm.configPath = "/home/user/.config/greencurve/config.ini";
        vm.status = "test";
        vm.probeCompleted = false;
        TuiLayout layout;
        build_tui_layout(vm, &layout);

        if (layout.requiredRows != (int)layout.lines.size()) return 200;
        if (layout.actions.empty()) return 201;
        if (layout.requiredCols <= 0) return 202;

        bool sawApply = false, sawApplyReset = false, sawQuit = false;
        for (size_t ai = 0; ai < layout.actions.size(); ai++) {
            const ClickAction& a = layout.actions[ai];
            if (a.type == ACTION_APPLY) sawApply = true;
            if (a.type == ACTION_APPLY_RESET) sawApplyReset = true;
            if (a.type == ACTION_QUIT) sawQuit = true;
            if (a.y1 < 1 || a.y1 > (int)layout.lines.size()) return 203;
            if (a.y1 != a.y2) return 204;
            const std::string& row = layout.lines[a.y1 - 1];
            if (a.byteStart < 0 || a.byteLen < 2) return 205;
            if (a.byteStart + a.byteLen > (int)row.size()) return 206;
            if (row[a.byteStart] != '[') return 207;                      // hitbox lands on a bracket
            if (row[a.byteStart + a.byteLen - 1] != ']') return 208;
            int expectStart = tui_display_columns(row.substr(0, a.byteStart)) + 1;
            int expectEnd = tui_display_columns(row.substr(0, a.byteStart + a.byteLen));
            if (a.x1 != expectStart) return 209;                          // display column, not byte offset
            if (a.x2 != expectEnd) return 210;
            if (a.x2 > layout.requiredCols) return 211;                   // interactive line never wraps
        }
        if (!sawApply || !sawApplyReset || !sawQuit) return 212;

        // Explicit multibyte width proof: '°' (0xC2 0xB0) is one display column.
        std::string degLine = "ab\xC2\xB0""cd";  // a b ° c d -> 5 columns, 'c' at byte 4
        if (tui_display_columns(degLine) != 5) return 213;
        if (tui_column_to_byte_offset(degLine, 4) != 4) return 214;
    }

    DeleteCriticalSection(&g_configLock);
    return 0;
}
'''
    tmp = prepare_work_subdir("test")
    try:
        harness_path = os.path.join(tmp, "fan_curve_regression.cpp")
        test_exe = os.path.join(tmp, "fan_curve_regression.exe" if sys.platform == "win32" else "fan_curve_regression")
        with open(harness_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(harness)
        compiler = LLVM_MINGW_CLANG if sys.platform == "win32" else ZIG_EXE
        cmd = [
            compiler,
        ]
        # llvm-mingw clang++ takes flags directly; zig c++ needs the subcommand
        if sys.platform != "win32":
            cmd.append("c++")
        cmd.extend([
            "-std=c++17",
            "-DNDEBUG",
            "-fno-exceptions",
            "-fno-rtti",
            f"-I{SOURCE_DIR}",
            "-o",
            test_exe,
            harness_path,
            os.path.join(SOURCE_DIR, "fan_curve.cpp"),
            os.path.join(SOURCE_DIR, "config_utils.cpp"),
            os.path.join(SOURCE_DIR, "app_shared.cpp"),
            os.path.join(SOURCE_DIR, "service_acl.cpp"),
            os.path.join(SOURCE_DIR, "vf_backends.cpp"),
        ])
        if sys.platform == "win32":
            cmd.append(os.path.join(SOURCE_DIR, "platform_win32.cpp"))
        if extra_flags:
            cmd.extend(extra_flags)
        if sys.platform == "win32":
            cmd.extend(["-static", "-luser32", "-lgdi32", "-luuid", "-ladvapi32", "-lshell32"])
        print("Compiling pure regression tests")
        result = subprocess.run(cmd, cwd=SCRIPT_DIR)
        if result.returncode != 0:
            print("Regression test compilation FAILED")
            sys.exit(result.returncode)
        config_path = os.path.join(tmp, "config_roundtrip.ini")
        print("Running pure regression tests")
        test_env = os.environ.copy()
        if sys.platform == "win32":
            # ASan links the llvm-mingw dynamic sanitizer runtime; keep the
            # portable toolchain's bin directory on PATH for the test process.
            llvm_bin = os.path.dirname(LLVM_MINGW_CLANG)
            test_env["PATH"] = llvm_bin + os.pathsep + test_env.get("PATH", "")
        result = subprocess.run([test_exe, config_path], cwd=SCRIPT_DIR, env=test_env)
        if result.returncode != 0:
            print(f"Regression tests FAILED ({result.returncode})")
            sys.exit(result.returncode)
        run_source_regression_checks()
        print("Regression tests passed")
    finally:
        cleanup_work_subdir(tmp)


def require_text(path, needle, label):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if needle not in text:
        print(f"Regression source check FAILED: {label}")
        sys.exit(1)


def forbid_text(path, needle, label):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if needle in text:
        print(f"Regression source check FAILED (must be absent): {label}")
        sys.exit(1)


def require_order(path, first, second, label):
    """Assert that `first` appears before `second` in the file (both required)."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    fi = text.find(first)
    si = text.find(second)
    if fi < 0 or si < 0 or fi >= si:
        print(f"Regression source check FAILED (order): {label}")
        sys.exit(1)


def require_app_version_fallback_in_sync():
    """The compiled version string (Windows title bar, logs, Linux banner) comes
    from the `#ifndef APP_VERSION` fallback macro in the headers — the build does
    NOT inject -DAPP_VERSION into the compiler.  A VERSION bump that forgets the
    header fallbacks therefore ships a binary still showing the old version (the
    exact 0.17 -> 0.17.1 title-bar drift).  Assert the header fallbacks match the
    VERSION file so this cannot recur silently.
    """
    import re
    for rel in ("app_shared.h", "linux_port.h"):
        path = os.path.join(SOURCE_DIR, rel)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        match = re.search(r'#define\s+APP_VERSION\s+"([^"]+)"', text)
        if not match:
            print(f"Regression source check FAILED: APP_VERSION fallback missing in {rel}")
            sys.exit(1)
        if match.group(1) != APP_VERSION:
            print(f"Regression source check FAILED: {rel} APP_VERSION fallback "
                  f"'{match.group(1)}' != VERSION '{APP_VERSION}' (bump the header fallback too)")
            sys.exit(1)


def warn_oversized_source_files(soft_limit=800):
    """F-MAINT-1: warn about source files exceeding the ~600-800 line guideline.

    Non-fatal: prints a maintainability warning so file growth stays visible and
    new oversized files get noticed. app_shared.h (shared protocol/type header) is
    intentionally exempt — splitting it is high-risk/low-value.
    """
    exempt = {"app_shared.h"}
    oversized = []
    for name in sorted(os.listdir(SOURCE_DIR)):
        if not (name.endswith(".cpp") or name.endswith(".h")):
            continue
        if name in exempt:
            continue
        path = os.path.join(SOURCE_DIR, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                lines = sum(1 for _ in handle)
        except OSError:
            continue
        if lines > soft_limit:
            oversized.append((lines, name))
    if oversized:
        oversized.sort(reverse=True)
        print(f"NOTE: {len(oversized)} source file(s) exceed the ~{soft_limit}-line guideline (F-MAINT-1):")
        for lines, name in oversized:
            print(f"  {lines:5d}  source/{name}")


def run_source_regression_checks():
    warn_oversized_source_files()
    require_app_version_fallback_in_sync()
    main_cpp = os.path.join(SOURCE_DIR, "main.cpp")
    entry_cpp = os.path.join(SOURCE_DIR, "entry.cpp")
    diagnostics_cpp = os.path.join(SOURCE_DIR, "main_diagnostics.cpp")
    secure_write_cpp = os.path.join(SOURCE_DIR, "main_secure_write.cpp")
    service_ipc_cpp = os.path.join(SOURCE_DIR, "main_service_ipc.cpp")
    service_server_cpp = os.path.join(SOURCE_DIR, "main_service_server.cpp")
    config_utils_cpp = os.path.join(SOURCE_DIR, "config_utils.cpp")
    fan_curve_cpp = os.path.join(SOURCE_DIR, "fan_curve.cpp")
    config_profiles_ui_cpp = os.path.join(SOURCE_DIR, "config_profiles_ui.cpp")
    desired_settings_helpers_cpp = os.path.join(SOURCE_DIR, "desired_settings_helpers.cpp")
    config_profile_repair_cpp = os.path.join(SOURCE_DIR, "config_profile_repair.cpp")
    gpu_backend_apply_cpp = os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp")
    main_gpu_state_cpp = os.path.join(SOURCE_DIR, "main_gpu_state.cpp")
    main_state_sync_cpp = os.path.join(SOURCE_DIR, "main_state_sync.cpp")
    main_tail_diagnostics_cpp = os.path.join(SOURCE_DIR, "main_tail_diagnostics.cpp")
    main_shell_cpp = os.path.join(SOURCE_DIR, "main_shell.cpp")
    main_fan_runtime_cpp = os.path.join(SOURCE_DIR, "main_fan_runtime.cpp")
    main_gpu_front_cpp = os.path.join(SOURCE_DIR, "main_gpu_front.cpp")
    runtime_nvml_cpp = os.path.join(SOURCE_DIR, "main_runtime_nvml.cpp")
    gpu_backend_cpp = os.path.join(SOURCE_DIR, "gpu_backend.cpp")
    main_runtime_control_cpp = os.path.join(SOURCE_DIR, "main_runtime_control.cpp")
    main_runtime_gpu_cpp = os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp")
    main_service_runtime_cpp = os.path.join(SOURCE_DIR, "main_service_runtime.cpp")
    main_service_vf_drift_cpp = os.path.join(SOURCE_DIR, "main_service_vf_drift.cpp")
    main_service_persist_cpp = os.path.join(SOURCE_DIR, "main_service_persist.cpp")
    main_service_recovery_cpp = os.path.join(SOURCE_DIR, "main_service_recovery.cpp")
    sessions_cpp = os.path.join(SOURCE_DIR, "main_service_sessions.cpp")
    main_service_install_cpp = os.path.join(SOURCE_DIR, "main_service_install.cpp")
    ui_main_cpp = os.path.join(SOURCE_DIR, "ui_main.cpp")
    ui_main_window_cpp = os.path.join(SOURCE_DIR, "ui_main_window.cpp")
    vf_backends_cpp = os.path.join(SOURCE_DIR, "vf_backends.cpp")
    gpu_core_h = os.path.join(SOURCE_DIR, "gpu_core.h")
    # The platform-neutral data model (NVAPI/NVML types, VfBackendSpec,
    # DesiredSettings + IPC validator, ServiceRequest/Response, NvmlApi) was
    # split out of app_shared.h into gpu_core.h so the Linux backend can share
    # it.  The "shared header surface" source checks below assert invariants
    # that may now live in EITHER header, so point shared_h/app_shared_h at a
    # concatenation of both files.
    _shared_surface = os.path.join(BUILD_WORK_DIR, "_shared_header_surface.h")
    os.makedirs(BUILD_WORK_DIR, exist_ok=True)
    with open(_shared_surface, "w", encoding="utf-8", errors="ignore") as _sf:
        for _h in (os.path.join(SOURCE_DIR, "app_shared.h"), gpu_core_h):
            with open(_h, "r", encoding="utf-8", errors="ignore") as _hf:
                _sf.write(_hf.read())
                _sf.write("\n")
    shared_h = _shared_surface
    app_shared_h = shared_h
    app_shared_cpp = os.path.join(SOURCE_DIR, "app_shared.cpp")
    build_script = os.path.join(SCRIPT_DIR, "build.py")
    build_py_text = build_script
    gitignore = os.path.join(SCRIPT_DIR, ".gitignore")

    require_text(shared_h, "APP_DEBUG_DEFAULT_ENABLED 1", "debug logging remains default-on")
    require_text(shared_h, "APP_TITLE           APP_NAME \" v\" APP_VERSION", "plain title macro exists")
    require_text(shared_h, "SERVICE_PROTOCOL_VERSION = 8", "service protocol version bumped for fixed-width IPC bools")
    require_text(shared_h, "typedef gc_u8 gc_bool8", "IPC bool fields use a fixed-width one-byte type")
    require_text(shared_h, "canonicalize_gc_bool8", "IPC bool fields are canonicalized at trust boundaries")
    require_text(shared_h, "validate_service_response_for_ipc", "service responses are canonicalized before GUI use")
    require_text(shared_h, "resetOcBeforeApply", "GUI apply reset-before-apply protocol flag exists")
    # F-SEC-4: the IPC trust-boundary validator must clamp every field that can
    # reach an array index, the fan policy switch, or a fan-speed write.
    require_text(shared_h, "d->lockCi >= VF_NUM_POINTS", "IPC validator clamps lockCi to array bounds")
    require_text(shared_h, "d->gpuOffsetExcludeLowCount > VF_NUM_POINTS", "IPC validator clamps selective-offset exclude count")
    require_text(shared_h, "d->fanMode > FAN_MODE_CURVE", "IPC validator clamps fan mode to a valid enum value")
    require_text(shared_h, "GpuAdapterInfo", "GPU adapter identity protocol exists")
    require_text(shared_h, "APP_BUILD_NUMBER", "build number define exists")
    require_text(shared_h, "serviceBuildNumber", "service response carries build number")
    require_text(shared_h, "serviceVersion[32]", "service response carries version")
    require_text(diagnostics_cpp, "protocol=%lu", "session marker logs IPC protocol")
    require_text(diagnostics_cpp, "build=%lu", "session marker logs build number")
    require_text(diagnostics_cpp, "close_debug_log_file", "debug log file cleanup exists")
    require_text(diagnostics_cpp, "open_debug_log_file_locked", "debug log file open helper exists")
    require_text(diagnostics_cpp, "green_curve_unhandled_exception_filter", "crash filter exists")
    require_text(diagnostics_cpp, "MiniDumpWriteDump", "crash filter writes minidump")
    require_text(secure_write_cpp, "write_all_to_handle", "file writes use size_t-safe chunked write helper")
    require_text(main_cpp, "SERVICE_PIPE_SERVER_IO_TIMEOUT_MS", "service pipe server I/O timeout exists")
    require_text(service_server_cpp, "CancelIoEx(pipe, &ov)", "stalled pipe operations are cancellable")
    require_text(service_server_cpp, "response.serviceBuildNumber", "service responses include build number")
    require_text(service_server_cpp, "restricted ACL creation returned no descriptor", "service pipe creation fails closed without ACL")
    require_text(service_server_cpp, "FATAL failed to create pipe server thread", "service fails closed when pipe server thread cannot start")
    require_text(main_service_install_cpp, "stop_service_for_binary_update", "service repair stops old service before replacing binary")
    require_text(main_service_install_cpp, "SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS", "service repair can stop installed service")
    require_text(service_ipc_cpp, "service_client_ping: identity mismatch", "GUI rejects mismatched service version identity")
    require_text(service_ipc_cpp, "compatible build mismatch accepted", "GUI accepts compatible service build-number drift")
    require_text(service_ipc_cpp, "backgroundServiceError", "service ping failures are surfaced to the GUI")
    require_text(service_ipc_cpp, "GetNamedPipeServerProcessId", "GUI verifies service pipe server PID")
    require_text(service_ipc_cpp, "SetNamedPipeHandleState", "service pipe message mode is checked")
    require_text(service_ipc_cpp, "Service response protocol mismatch", "service responses are validated before use")
    require_text(service_ipc_cpp, "WaitNamedPipeW(pipeName, waitSlice)", "service pipe connect retries on ERROR_PIPE_BUSY")
    require_text(service_ipc_cpp, "ensure_secure_service_binary_path", "service install uses hardened adjacent service binary path")
    require_text(service_ipc_cpp, "CopyFileW(sourcePath, tempPath", "service binary is staged before install")
    require_text(service_ipc_cpp, "get_current_executable_directory_w", "service install resolves the current executable directory")
    require_text(service_ipc_cpp, "get_service_binary_path_from_scm(expectedPath", "service pipe identity compares against the SCM-registered service binary")
    require_text(service_ipc_cpp, "apply_protected_service_dir_dacl", "service install hardens the staging directory DACL")
    require_text(service_ipc_cpp, "service_binary_dacl_is_hardened(targetPath)", "service install verifies the staged binary DACL")
    require_text(service_ipc_cpp, "restore_inherited_dacl(installDir", "service uninstall restores adjacent directory DACL")
    require_text(service_ipc_cpp, "directory_path_is_root_or_share_root_w", "service uninstall skips overly broad root/share-root DACL restore")
    require_text(service_server_cpp, "Requested GPU identity no longer matches", "service validates requested GPU PCI identity before mutation")
    require_order(service_server_cpp,
        "if (request->targetGpu.nvapiIndex >= MAX_GPU_ADAPTERS)",
        "g_app.selectedGpuIndex = live.nvapiIndex",
        "service validates requested GPU index before mutating selected GPU state")
    require_text(main_service_persist_cpp, "ServiceRestartReapplySnapshot", "restart-reapply snapshot persists target GPU identity")
    require_text(main_service_persist_cpp, "targetGpu", "restart-reapply snapshot carries target GPU")
    require_text(main_service_recovery_cpp, "target GPU unavailable", "restart reapply skips when target GPU cannot be matched")
    require_text(main_state_sync_cpp, "service_sid_string_from_token", "service user path cache resolves the caller SID")
    require_text(main_state_sync_cpp, "g_serviceUserPathsSid", "service user path cache keys by session id plus SID")
    require_text(os.path.join(SOURCE_DIR, "main_service_sessions.cpp"), "g_lastAppliedSessionSid", "session reapply debounce keys by session id plus SID")
    require_text(os.path.join(SOURCE_DIR, "main_service_sessions.cpp"), "service_last_applied_session_matches", "session reapply debounce compares the full user identity")
    require_text(diagnostics_cpp, "crash_artifact_data_dir", "service crash artifacts route through a process-appropriate data directory")
    require_text(diagnostics_cpp, "resolve_service_machine_data_dir", "service crash artifacts use the machine service data directory")
    require_text(config_profile_repair_cpp, "savedOffsetMagnitude = savedOffset < 0 ? -(long long)savedOffset", "profile repair avoids abs(INT_MIN) overflow")
    require_text(service_ipc_cpp, "pl_append_quoted_arg_w", "elevated helper command lines use argv-compatible quoting")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "pl_append_quoted_arg_w", "GUI elevated helper command lines use argv-compatible quoting")
    require_text(build_script, "_verify_cached_tool_binary", "cached tool binaries are verified against trusted pinned digests")
    require_text(build_script, "LLVM_MINGW_CLANG_SHA256", "llvm-mingw executable digest is pinned")
    require_text(service_ipc_cpp, "wait_for_helper_process_bounded", "elevated helper waits are bounded")
    require_text(config_profiles_ui_cpp, "maybe_load_selected_profile_to_gui_without_apply", "startup restores selected profile into GUI without applying")
    require_text(config_profiles_ui_cpp, "repair needed", "broken installed service advertises repair state")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "Repair and restart the background service", "broken installed service click repairs instead of removing")
    require_text(config_profiles_ui_cpp, "GPU settings were not applied", "startup selected profile restore is GUI-only")
    require_text(desired_settings_helpers_cpp, "desired_settings_match_active_service_intent", "profile intent can be compared to the service active desired state")
    require_text(config_profiles_ui_cpp, "already active in background service; skipping reset-before-apply", "app-start auto-load skips disruptive reset/apply when service already owns the same intent")
    require_order(config_profiles_ui_cpp,
        "desired_settings_match_active_service_intent(&desired",
        "desired.resetOcBeforeApply = true;",
        "app-start active-service match is checked before reset-before-apply")
    require_text(gpu_backend_apply_cpp, "applying anyway by design", "memory offsets outside reported range are still attempted (F-DOM-1: not gated)")
    require_text(main_gpu_state_cpp, "live_selective_gpu_offset_matches_requested_shape", "persisted selective GPU offset is verified against live VF shape")
    require_text(main_gpu_state_cpp, "runtime selective: ignoring persisted request", "stale persisted selective GPU offset is ignored")
    require_text(main_gpu_state_cpp, "non-selective request clears runtime state", "uniform GPU offsets clear runtime selective state")
    require_text(main_gpu_state_cpp, 'debug_log_on_change("current_applied_gpu_offset_mhz: not Blackwell', "stable non-Blackwell GPU offset diagnostic is change-gated")
    forbid_text(main_gpu_state_cpp, 'debug_log("current_applied_gpu_offset_mhz: not Blackwell', "stable non-Blackwell GPU offset diagnostic must not spam every poll")
    require_text(main_runtime_gpu_cpp, 'debug_log_on_change("populate_global_controls: dirty=', "global control refresh diagnostics are change-gated")
    require_text(gpu_backend_apply_cpp, "interactive && !g_app.isServiceProcess", "service apply does not inherit stale GUI lock state")
    require_text(gpu_backend_apply_cpp, "post-apply lock clear: no lock requested", "service no-lock applies clear stale lock markers")
    require_text(gpu_backend_apply_cpp, "reset_oc_before_gui_apply", "GUI OC applies reset stale OC baseline before applying")
    require_text(gpu_backend_apply_cpp, "Restoring the existing VF curve after the memory offset did not verify", "VF preservation failures are reported")
    require_text(gpu_backend_apply_cpp, "non-tail %s point %d actual %u MHz != target", "non-tail readback artifacts are accepted only for verification")
    require_text(gpu_backend_apply_cpp, "keeping strict lock target", "lock tail readback mismatches do not mutate requested intent")
    require_text(gpu_backend_apply_cpp, "stockBase = (long long)originalCurveFreqkHz", "correction loop uses stock base for non-tail explicit points to avoid cumulative offset bug")
    require_text(gpu_backend_apply_cpp, "post-apply curve: ci=%d actual=%u", "post-apply curve state dump detects weird shifts")
    require_text(gpu_backend_apply_cpp, "not rewriting tail above lock", "monotonicity enforcement never raises the locked tail above the requested lock")
    require_text(os.path.join(SOURCE_DIR, "main_shell.cpp"), "skipping stale lock at ci=%d (lockedFreq=0", "stale lock skip only when lockedFreq=0, not when == liveMHz")
    require_text(gpu_backend_apply_cpp, "post-apply tail bookends", "post-apply logs tail bookends even when within tolerance")
    require_text(gpu_backend_apply_cpp, "post-apply tail: ci=%d actual=%u", "post-apply logs tail drifts > 2 MHz even when within tolerance")
    require_text(gpu_backend_apply_cpp, "high offset warning summary", "large VF offset diagnostics are aggregated")
    require_text(gpu_backend_cpp, "update_tray_icon", "VF/GPU offset applies update tray icon from GUI-side apply path")
    require_text(gpu_backend_cpp, "parse_mhz_value_prefix", "nvidia-smi clock parsing is strict")
    require_text(main_fan_runtime_cpp, "if (g_app.freqOffsets[i] != 0) return true;", "live_state_has_custom_oc checks freqOffsets without vfBackend guard")
    require_text(runtime_nvml_cpp, "rollback_changed_fans", "manual multi-fan writes roll back partial failures")
    require_text(runtime_nvml_cpp, "nvml_select_device_for_selected_gpu", "NVML device is matched to selected GPU")
    require_text(secure_write_cpp, "write_text_file_atomic_service", "service file writes use hardened writer")
    require_text(ui_main_cpp, "gpuSelectY = dp(10)", "GPU selector lives in the graph header gap")
    require_text(main_gpu_state_cpp, "best-effort support for a new NVIDIA GPU family", "unrecognized GPU warning explains best-effort writes")
    require_text(main_gpu_front_cpp, "vf_backend_for_architecture(g_app.gpuArchitecture", "Windows backend selection reuses the shared architecture mapping")
    require_text(main_gpu_front_cpp, "nvapi_read_gpu_metadata: archStatus=", "GPU metadata logging includes NVAPI architecture query status")
    require_text(main_gpu_front_cpp, "retaining last known backend for same PCI adapter", "transient metadata failures keep the last known backend for the same GPU")
    require_text(vf_backends_cpp, "case NV_GPU_ARCHITECTURE_GB200: fam = GPU_FAMILY_BLACKWELL", "Blackwell maps to a known non-best-effort backend")
    require_text(main_shell_cpp, "preserving requested value", "config memory offsets are not clamped to reported range")
    require_text(main_gpu_state_cpp, "current_green_curve_fan_intent_mode", "fan state has a Green Curve-owned intent helper")
    require_text(main_state_sync_cpp, "external live fan policy observed fanIsAuto=0 gcIntent=Auto", "service snapshots preserve Auto intent when external fan control is manual")
    require_text(main_state_sync_cpp, "state->fanMode is Green Curve intent", "control-state fan mode is not treated as live driver fan policy")
    require_text(ui_main_window_cpp, "preserved visible GUI fan intent", "profile Save preserves the visible fan mode over live external policy")
    require_text(main_fan_runtime_cpp, "external live fan policy is %s while Green Curve intent is Auto", "fan initialization logs external manual policy without adopting it")
    # GUI lock-checkbox regressions (two user-reported bugs):
    #  (1) the FLATTEN tick must reuse the anti-aliased renderer shared by the themed
    #      checkboxes (service install / share-all-users / tray) instead of a jagged
    #      raw-GDI Polyline, so it does not look corrupted next to them.
    #  (2) the tri-state WM_COMMAND cycle must be gated on BN_CLICKED: the lock buttons
    #      are BS_OWNERDRAW + WS_TABSTOP, so an unfocused click first emits BN_SETFOCUS
    #      (and BN_KILLFOCUS on the old box); without the guard those focus notifications
    #      ran the cycle a second time, making one click skip FLATTEN straight to HARD.
    require_text(main_shell_cpp, "draw_checkbox_tick_smooth(hdc, &box, RGB(0xE8, 0xF2, 0xFF))", "lock FLATTEN tick uses the shared anti-aliased checkmark renderer")
    forbid_text(main_shell_cpp, "Polyline(hdc, pts, 3)", "lock checkbox no longer draws the jagged raw-GDI checkmark")
    require_text(ui_main_window_cpp, "HIWORD(wParam) == BN_CLICKED &&", "lock tri-state cycle is gated on BN_CLICKED so focus notifications cannot double-advance")
    require_text(ui_main_window_cpp, "lock checkbox cycle: vi=%d notify=", "lock checkbox cycle logs the notification code + state transition for diagnosis")
    require_text(ui_main_cpp, "lock_checkbox_subclass_proc", "lock checkbox subclass exists")
    require_text(ui_main_cpp, "WM_LBUTTONDBLCLK", "lock checkbox double-click cannot advance the tri-state twice")
    require_text(runtime_nvml_cpp, "parse_cli_point_arg_w(arg, &idx)", "CLI point parsing is strict")
    require_text(entry_cpp, "set_main_window_title", "window caption helper exists")
    require_text(entry_cpp, "SetWindowTextA", "window caption uses ANSI text write")
    require_text(entry_cpp, "RegisterClassExA", "main window uses ANSI class registration")
    require_text(entry_cpp, "CreateWindowExA", "main window uses ANSI creation path")
    require_text(entry_cpp, "--set-machine-logon-slot", "CLI supports setting machine default logon slot")
    require_text(entry_cpp, "--clear-machine-logon-slot", "CLI supports clearing machine default logon slot")
    require_text(config_profiles_ui_cpp, "update_share_all_users_check_state", "GUI updates the share-with-all-users checkbox state")
    require_text(config_profiles_ui_cpp, "refresh_machine_logon_slot_cache", "GUI refreshes machine logon slot cache")
    require_text(main_fan_runtime_cpp, "FindWindowA", "single-instance lookup uses ANSI class matching")
    require_text(build_script, "--check", "build check flag exists")
    require_text(build_script, "--test", "test flag exists")
    require_text(build_script, "compile_commands.json", "LSP database generation exists")
    require_text(gitignore, "*.7z", "release archives are ignored")
    # NOTE: ssp_glue.cpp provides the runtime symbols (__stack_chk_guard,
    # __stack_chk_fail) that the MinGW CRT omits on Windows, making stack-
    # protector canaries functional.  Keep the flag at all times.
    require_text(build_script, "-fstack-protector-strong", "stack protector flag enables canary emission with ssp_glue.cpp")
    require_text(build_script, "-mguard=cf", "Control Flow Guard flag enables CFG for Windows")
    require_text(build_script, "-fcf-protection=full", "CET/Shadow Stack instrumentation (endbr64) adds hardware-enforced control-flow integrity")
    require_text(build_script, "-flto", "Link-Time Optimization enables cross-module inlining and dead code elimination at link time")
    require_text(build_script, "--icf=safe", "Identical Code Folding merges identical functions to reduce binary size")
    require_text(build_script, "-ftrivial-auto-var-init=pattern", "auto-var-init pattern flag initializes stack variables")
    require_text(build_script, 'ZIG_EXE, "c++"', "arm64 Windows uses Zig to dodge the llvm-mingw aarch64 'misaligned ldr/str offset' link bug")
    require_text(build_script, '"-target", "aarch64-windows-gnu"', "arm64 Windows Zig build targets the correct triple")
    require_text(build_script, "-fno-delete-null-pointer-checks", "null pointer check flag prevents deletion of null checks")
    require_text(build_script, "-fPIE", "Linux PIE hardening retained")
    require_text(build_script, "-Wl,-z,relro,-z,now", "Linux RELRO/BIND_NOW hardening retained")
    require_text(build_script, "-Wl,-z,noexecstack", "Linux non-executable stack hardening retained")
    # F-SEC-2: DLL-search hardening runs in initialize_process_mitigations(),
    # which both the GUI (entry.cpp) and service (main.cpp) entry points call
    # before any runtime LoadLibrary — blocks DLL planting of non-KnownDLLs.
    cfg_glue_cpp = os.path.join(SOURCE_DIR, "cfg_glue.cpp")
    require_text(cfg_glue_cpp, "SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32", "startup hardens the DLL search path against planting")
    require_text(cfg_glue_cpp, "SetDllDirectoryW(L\"\")", "startup removes the CWD from the DLL search path")
    # F-SEC-1: service install hardens the installed binary DACL (no non-admin
    # overwrite of a SYSTEM service binary) and uninstall reverts it so the user
    # can delete/replace the unregistered binary again.
    service_acl_cpp = os.path.join(SOURCE_DIR, "service_acl.cpp")
    require_text(service_ipc_cpp, "apply_protected_service_binary_dacl(targetPath", "service install hardens the installed binary DACL")
    require_text(service_ipc_cpp, "restore_inherited_dacl(targetPath", "service uninstall reverts the binary DACL to inherited")
    require_text(service_acl_cpp, "PROTECTED_DACL_SECURITY_INFORMATION", "binary DACL hardening disables inheritance")
    require_text(service_acl_cpp, "(A;;0x1200a9;;;BU)", "binary DACL grants BUILTIN\\Users read+execute only")
    # F-SEC-6: machine-wide default logon profile config is admin-writable and
    # user-readable, so non-admins can read the current default but cannot
    # tamper with it.
    require_text(service_acl_cpp, "apply_protected_machine_config_dacl", "machine config DACL helper exists")
    require_text(service_acl_cpp, "(A;;0x120089;;;BU)", "machine config DACL grants BUILTIN\\Users read only")
    require_text(service_ipc_cpp, "resolve_machine_config_path", "machine config path resolver exists")
    require_text(service_ipc_cpp, "get_machine_logon_slot", "machine logon slot reader exists")
    require_text(service_ipc_cpp, "set_machine_logon_slot", "machine logon slot writer exists")
    require_text(service_ipc_cpp, "clear_machine_logon_slot", "machine logon slot clearer exists")
    # F-SEC-5 / policy: program files live only under %LOCALAPPDATA%\\Green Curve.
    # The shared resolver and the one-time legacy cleanup are the only places
    # permitted to mention ProgramData; the per-process data/diagnostics paths must
    # not. (Service LocalAppData = SYSTEM profile, which is admin-only.)
    state_sync_cpp = os.path.join(SOURCE_DIR, "main_state_sync.cpp")
    service_runtime_cpp = os.path.join(SOURCE_DIR, "main_service_runtime.cpp")
    require_text(state_sync_cpp, "resolve_service_machine_data_dir", "service machine data dir resolves under LocalAppData")
    require_text(state_sync_cpp, "service_cleanup_legacy_programdata", "legacy ProgramData directory cleanup exists")
    require_text(service_server_cpp, "service_cleanup_legacy_programdata()", "service startup runs the legacy ProgramData cleanup")
    forbid_text(service_runtime_cpp, "ProgramData", "service runtime stores files under LocalAppData, not ProgramData")
    forbid_text(diagnostics_cpp, "ProgramData", "diagnostics stores files under LocalAppData, not ProgramData")
    # F-DOM-1 / release 0.16: only unrecognized future GPU families are
    # best-effort.  The warning shows once per GUI session and can be disabled by
    # the user through a persistent [warnings] flag; Pascal/Turing/Ampere are
    # now treated as tested known backends.
    require_text(main_gpu_front_cpp, "hide_unrecognized_gpu_warning", "unrecognized GPU warning can be disabled by the user")
    require_text(os.path.join(SOURCE_DIR, "main_gpu_state.cpp"), "pszVerificationText", "TaskDialog warning exposes a do-not-show-again checkbox")
    forbid_text(os.path.join(SOURCE_DIR, "main_gpu_state.cpp"), "hide_best_guess_warning_", "old best-guess warning flag is not resurrected")

    # F-04-001: Pipe server identity verification beyond PID
    require_text(service_ipc_cpp, "does not match expected", "pipe server executable path is verified against expected service binary")
    require_text(service_ipc_cpp, "cannot verify server binary path", "pipe server identity accepts PID match when image path cannot be queried")

    # F-04-002: LocalSystem file-write parent directory verification
    require_text(secure_write_cpp, "parent dir verified", "service file-write verifies parent directory before temp creation")
    require_text(secure_write_cpp, "FILE_FLAG_OPEN_REPARSE_POINT", "service file-write opens parent without following reparse points")

    # F-01-002: Fan failure triggers rollback of earlier hardware writes
    require_text(gpu_backend_apply_cpp, "fan failure triggered rollback", "fan apply failure triggers rollback of earlier hardware writes")

    # F-01-003: Multi-GPU ordinal fallback blocked
    require_text(runtime_nvml_cpp, "refusing ordinal fallback", "multi-GPU ordinal fallback is blocked when PCI identity is available")

    # F-06-001: Lock capture always reads edit box, not just when lockedFreq <= 0
    require_text(main_runtime_control_cpp, "get_window_text_safe(g_app.hEditsMhz[g_app.lockedVi]", "lock capture reads edit box unconditionally")

    # F-02-001: Fan apply validates before mutating runtime state
    require_text(main_fan_runtime_cpp, "fan auto write", "fan auto mode is applied before stopping runtime")
    require_text(main_fan_runtime_cpp, "restored driver auto, runtime stopped", "fan auto apply logs successful restore before stopping runtime")

    # F-15-002: Multi-fan rollback tracks failures
    require_text(runtime_nvml_cpp, "rollbackFailures", "multi-fan rollback tracks individual rollback failures")

    # F-02-003: Fan curve hysteresis prevents timed-reapply override
    require_text(main_fan_runtime_cpp, "hysteresis blocked the drop", "fan curve hysteresis blocks timed-reapply override")

    # F-03-001: Checked arithmetic for selective offset
    require_text(main_gpu_state_cpp, "rejecting out-of-range gpuOffsetMHz", "persisted selective GPU offset is range-checked before use")

    # F-02-002: Fan thread handle preserved on timeout
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "thread handle preserved", "fan thread handle is preserved on timeout to prevent replacement")

    # F-01-001: Heap-based VF curve buffers (no large stack allocations)
    require_text(os.path.join(SOURCE_DIR, "app_shared.h"), "struct HeapBuffer", "HeapBuffer is defined in shared header")
    require_text(gpu_backend_cpp, "HeapBuffer buf(", "VF curve read uses heap buffer")
    require_text(gpu_backend_cpp, "HeapBuffer buf(", "VF curve offset read uses heap buffer")
    require_text(gpu_backend_cpp, "HeapBuffer buf(", "VF point write uses heap buffer")

    # F-01-003 / F-15-012: active-session enforcement is SERVER-SIDE (caller
    # session resolved from the pipe handle), so the pipe ACL is user-agnostic
    # (authenticated local users read+write) and must NOT bake a per-user SID —
    # a baked SID locked the next active user out of connecting after an account
    # switch until reboot.
    require_text(service_runtime_cpp, "Service control is restricted to the active interactive session", "service rejects non-active-session callers (F-SEC-3 server-side)")
    require_text(service_runtime_cpp, "get_pipe_client_identity", "caller session is resolved from the pipe handle, not the payload")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "(A;;GRGW;;;AU)", "pipe ACL admits authenticated local users read+write")
    forbid_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "GRGW;;;%s", "pipe ACL must not bake a per-user console SID (stale-ACL lockout)")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "cannot create restricted ACL, deferring", "pipe creation fails closed when the SD cannot be built")
    # F-15-003: Service listens for Windows session logon events so it can apply
    # a machine-wide default profile for users who have no per-user logon slot.
    require_text(service_server_cpp, "SERVICE_ACCEPT_SESSIONCHANGE", "service accepts session-change notifications")
    require_text(sessions_cpp, "WTS_SESSION_LOGON", "service handles WTS_SESSION_LOGON")
    require_text(service_runtime_cpp, "service_session_logon_resolve_and_load_profile", "service resolves per-user or machine default profile on logon")
    # Active-user session router: the service applies the ACTIVE session's
    # resolved profile on real logon/logoff/console-connect/disconnect events
    # (FUS-safe, debounced).  A plain service start while a user is already
    # logged in must stay non-mutating; installing/repairing the service should
    # not silently apply that user's logon slot.
    require_text(sessions_cpp, "service_handle_session_change", "active-user session-change router exists")
    require_text(sessions_cpp, "session_event_relevant_for_active_user", "session router filters events that change the active user")
    # Fast Startup / autologon safety net: on a BOOT auto-start where the session
    # is already active (no live WTS_SESSION_LOGON was delivered), the no-snapshot
    # startup coordinator reconciles that session's logon profile ONCE, reusing the
    # real logon apply path.  This MUST stay gated so a --manual GUI/CLI start
    # (install / repair / restart) is non-mutating and an SCM crash-restart within
    # the same boot cannot re-drive it.  The gating decision is pure + unit-tested
    # (should_reconcile_active_session_at_boot); the marker makes it at-most-once
    # per boot; --manual is threaded from StartService through to service_main.
    require_text(sessions_cpp, "service_maybe_reconcile_active_session_at_boot", "boot reconcile safety net exists for already-active sessions")
    require_text(sessions_cpp, "should_reconcile_active_session_at_boot", "boot reconcile uses the pure, unit-tested gating decision")
    require_text(sessions_cpp, "g_serviceManualStart", "boot reconcile is suppressed for --manual (GUI/CLI) starts")
    require_text(sessions_cpp, "service_apply_profile_for_session", "boot reconcile reuses the real logon apply+debounce path")
    require_text(main_service_recovery_cpp, "service_maybe_reconcile_active_session_at_boot", "no-snapshot startup coordinator invokes the boot reconcile")
    require_text(main_service_persist_cpp, "service_boot_reconcile_already_done", "boot reconcile is at-most-once per boot (persisted marker)")
    require_text(main_service_persist_cpp, "service_mark_boot_reconcile_done", "boot reconcile stamps the per-boot marker")
    require_text(main_service_install_cpp, "L\"--manual\"", "GUI/CLI service start passes --manual so it stays non-mutating")
    require_text(service_server_cpp, "--manual", "service_main reads the --manual manual-start signal")
    require_text(service_server_cpp, "service_handle_session_change", "SESSIONCHANGE handler routes through the active-user router")
    require_text(service_server_cpp, "g_servicePipeRecycleEvent", "pipe ACL is recycled on active-user change")
    # Per-user logon task is registered for the REQUESTING user, not the
    # approving admin, when the elevated helper runs on their behalf.
    require_text(service_ipc_cpp, "--for-user", "elevated startup-task helper forwards the requesting user")
    require_text(main_runtime_control_cpp, "set_forced_startup_user_sam", "startup task can be scoped to the requesting user")
    # F-15-004: The GUI requests UAC elevation for the machine-wide default
    # operation instead of requiring the whole GUI to be run as administrator.
    require_text(ui_main_window_cpp, "lpVerb = L\"runas\"", "machine logon button requests UAC elevation")
    require_text(ui_main_window_cpp, "ShellExecuteExW", "machine logon button uses wide ShellExecute with argv-compatible quoting")
    # F-15-005: the shared profile bank lives at a fixed %ProgramData% known
    # folder (all-users-readable, admin-write), NOT next to the service binary.
    # The SCM binary-path parser is retained only for the one-time legacy
    # machine.ini migration and the user-profile install warning.
    require_text(service_ipc_cpp, "FOLDERID_ProgramData", "shared bank path resolves under %ProgramData%")
    require_text(service_ipc_cpp, "migrate_legacy_machine_config", "one-time migration from legacy machine.ini location exists")
    require_text(service_server_cpp, "migrate_legacy_machine_config()", "service startup runs the legacy machine.ini migration")
    require_text(service_acl_cpp, "apply_protected_machine_config_dir_dacl", "shared bank directory DACL helper exists")
    # Anti-squat: the default %ProgramData% ACL lets standard users create
    # subfolders, so the service hardens %ProgramData%\Green Curve at boot (before
    # any login) to prevent a user pre-creating and planting a hostile bank file.
    require_text(service_ipc_cpp, "secure_shared_bank_at_startup", "service hardens the shared bank at boot (anti-squat)")
    require_text(service_server_cpp, "secure_shared_bank_at_startup()", "service runs shared-bank hardening at startup")
    require_text(service_ipc_cpp, "get_service_binary_directory_from_scm", "SCM binary dir resolver retained for migration/warning")
    require_text(service_ipc_cpp, "lpBinaryPathName", "SCM binary path parser retained for migration/warning")
    # F-15-006: Warn when the service is installed under a user profile, because
    # other users (including restricted/standard accounts) may not be able to
    # read/execute the GUI binary from another user's profile directory.
    require_text(service_ipc_cpp, "install_dir_is_under_user_profile_w", "user-profile install path detection exists")
    require_text(service_ipc_cpp, "service_install_dir_is_under_user_profile", "GUI can detect user-profile install path")
    require_text(service_ipc_cpp, "Install under %%ProgramFiles%% to make the application available to all users", "user-profile install warning text exists")
    # F-15-007: Shared profile bank. Full profile sections are copied into the
    # %ProgramData% shared bank so restricted users without their own config.ini
    # can still have the admin's saved profiles applied by the service.
    config_profiles_cpp = os.path.join(SOURCE_DIR, "config_profiles.cpp")
    require_text(config_profiles_cpp, "copy_profile_slot_to_machine_config", "shared bank copy helper exists")
    require_text(config_profiles_cpp, "clear_machine_profile_slot", "shared bank clear helper exists")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--publish-slot-to-machine", "CLI supports publishing a slot to the shared bank")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--clear-machine-slot", "CLI supports clearing a shared bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_PUBLISH_ID", "GUI advanced menu can publish a bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID", "GUI advanced menu can clear a bank slot")
    # F-15-008: One coherent "share with all users" action couples publishing the
    # slot data with setting it as the all-users default (the old footgun was
    # setting a default that resolved to an empty bank slot).
    require_text(config_profiles_cpp, "share_profile_slot_for_all_users", "coherent share helper publishes data AND sets the default")
    require_text(config_profiles_cpp, "unshare_profile_slot_for_all_users", "coherent unshare helper clears data AND the default")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--share-slot", "CLI supports the coherent share action")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--unshare-slot", "CLI supports the coherent unshare action")
    require_text(ui_main_window_cpp, "SHARE_ALL_USERS_CHECK_ID", "GUI has the share-with-all-users checkbox handler")
    # F-15-009: Any user can load the admin-published shared profiles on demand
    # (read-only) and apply them via the service, not just at logon.
    require_text(ui_main_window_cpp, "show_shared_profiles_menu", "GUI surfaces shared profiles for on-demand load")

    # F-15-010: Deleting a logon task that requires elevation (admin-created /
    # HighestAvailable) must fall back to the elevated helper instead of trusting
    # the schtasks exit code and reporting a dead-end "still exists" error.
    require_text(main_runtime_control_cpp, "outNeedsElevation", "direct startup-task path signals when elevation is required")
    require_text(main_runtime_control_cpp, "needs elevation", "startup-task delete that leaves the task present requests elevation")
    require_text(main_runtime_control_cpp, "schtasks /delete exit=", "schtasks delete logs its exit code for diagnosis")
    require_text(main_runtime_control_cpp, "schtasks /create exit=", "schtasks create logs its exit code for diagnosis")
    forbid_text(main_runtime_control_cpp, "exitCode != 0 && exitCode != 1", "startup-task delete must verify by state, not accept schtasks exit 1 as success")
    forbid_text(main_runtime_control_cpp, "Startup task still exists after delete", "dead-end delete error replaced by an elevation-aware fallback")
    # F-15-011: high-frequency idempotent debug lines are deduplicated (logged
    # only on change) to cut log spam without losing state transitions.
    require_text(os.path.join(SOURCE_DIR, "app_shared.h"), "debug_log_on_change", "log-on-change dedup helper exists")
    require_text(main_gpu_state_cpp, "debug_log_on_change(\"vf_curve_global_gpu_offset_supported", "GPU-offset support query is deduplicated")
    # F-15-013: GUI labels go through the ANSI Win32 path (CreateWindowExA "BUTTON"
    # / SetWindowTextA / DrawTextA), so a non-ASCII char in a button/label literal
    # renders as mojibake (e.g. U+2026 "…" -> "â€¦").  Keep the GUI-label files free
    # of the ellipsis char (use ASCII "..."). (debug_log strings may keep Unicode —
    # they go to a UTF-8 log file, not a window.)
    forbid_text(entry_cpp, "…", "shared-profiles button label must be ASCII (use ... not the … char)")
    forbid_text(config_profiles_ui_cpp, "…", "share/shared-profiles labels must be ASCII (use ... not the … char)")
    require_text(entry_cpp, "Shared profiles...", "shared-profiles button uses an ASCII ellipsis")
    # F-15-014: shared-only policy — a non-admin caller may only apply an admin-
    # published shared profile, enforced SERVER-SIDE (the service applies its own
    # copy of the named shared slot; the policy lives in the protected shared bank
    # and admin membership is resolved from the caller token, incl. deny-only).
    require_text(service_runtime_cpp, "token_is_local_admin", "service resolves caller machine-admin membership (incl. deny-only)")
    require_text(service_ipc_cpp, "restrict_non_admin_to_shared", "shared-only policy stored in the protected shared bank")
    require_text(service_ipc_cpp, "set_machine_restrict_policy", "shared-only policy writer is admin-gated")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "Your administrator restricts this PC to shared profiles", "service rejects non-admin custom OC under the policy")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "SERVICE_REQUEST_FLAG_SHARED_SLOT", "service applies its own copy of the named shared slot")
    require_text(service_ipc_cpp, "SERVICE_REQUEST_FLAG_SHARED_SLOT", "GUI tags an unmodified shared-profile apply as authoritative")
    require_text(entry_cpp, "--set-restrict-shared", "CLI toggles the shared-only policy")
    require_text(config_profiles_ui_cpp, "restricted_to_shared_profiles", "GUI surfaces the shared-only restriction to affected users")
    # Restricted-user logon auto-apply: a per-user "apply admin shared profile N
    # at logon" (logon_shared_slot) must survive the full-file config rewrites,
    # drive every logon path to the authoritative bank copy, and the service-side
    # resolver must enforce the policy (no per-user custom OC for non-admins),
    # closing the prior service-router bypass.
    require_text(config_profiles_cpp, "logon_shared_slot", "save/clear rewriters re-emit the per-user shared-logon choice")
    require_text(os.path.join(SOURCE_DIR, "app_shared.cpp"), "resolve_logon_profile_source", "pure logon-source policy decision is unit-testable")
    require_text(main_service_runtime_cpp, "service_session_user_is_local_admin", "service resolves the logon user's admin status for the policy")
    require_text(main_service_runtime_cpp, "resolve_logon_profile_source", "service logon resolver uses the shared policy decision")
    require_text(main_service_runtime_cpp, "logon_shared_slot", "service logon resolver honors the per-user shared-logon choice")
    require_text(main_service_runtime_cpp, "get_machine_restrict_policy", "service logon resolver checks the shared-only policy")
    require_text(config_profiles_ui_cpp, "apply_logon_shared_slot_if_configured", "GUI logon apply honors the per-user shared-logon choice")
    # The per-account logon choice (incl. admin shared profiles) lives in the single
    # unified "Apply profile after user log in" dropdown, tagged via CB_SETITEMDATA;
    # picking a shared entry sets logon_shared_slot and clears logon_slot.
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "LOGON_COMBO_SHARED_FLAG", "Logon dropdown offers admin shared profiles via item-data tags")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "CB_GETITEMDATA", "Logon dropdown handler decodes the selected item's meaning from item data")
    require_text(config_profiles_ui_cpp, "Shared profile %d (admin)", "Logon dropdown lists admin-published shared profiles")
    require_text(config_profiles_ui_cpp, "Admin default: Shared profile %d", "Logon dropdown shows the effective all-users default when the account has no choice")
    require_text(entry_cpp, "logon_shared_slot", "CLI silent logon apply honors the per-user shared-logon choice")

    # F-01-006: Sanitizer build support
    require_text(build_script, "--sanitizer", "sanitizer build flag exists")
    require_text(build_script, "SANITIZER_FLAGS", "sanitizer flags referenced")

    # F-01-007: Debug logging privacy notice
    require_text(os.path.join(SOURCE_DIR, "main_diagnostics.cpp"), "debug logging is enabled by default", "debug log privacy notice exists")

    # F-01-008: Move constructors on RAII wrappers
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "ScopedHandle(ScopedHandle&&", "ScopedHandle has move constructor")
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "ScopedGdiObject(ScopedGdiObject&&", "ScopedGdiObject has move constructor")
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "ScopedServiceHandle(ScopedServiceHandle&&", "ScopedServiceHandle has move constructor")

    # F-01-010: Response message defensive NUL termination
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "response.message[ARRAY_COUNT(response.message) - 1] = '\\0'", "defensive response NUL termination exists")

    # F-06-001: ScopedProcess RAII wrapper exists
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "struct ScopedProcess", "ScopedProcess RAII wrapper exists")
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "void assign(HANDLE proc, HANDLE thread)", "ScopedProcess.assign exists")
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "void assign_pipes(HANDLE read, HANDLE write)", "ScopedProcess.assign_pipes exists")
    require_text(os.path.join(SOURCE_DIR, "win32_raii.h"), "void terminate(DWORD exitCode", "ScopedProcess.terminate exists")

    # F-06-001: nvidia-smi callers use ScopedProcess
    require_text(gpu_backend_cpp, "ScopedProcess proc", "nvidia-smi clock read uses ScopedProcess")
    require_text(gpu_backend_cpp, "ScopedProcess proc", "nvidia-smi power limit uses ScopedProcess")

    # F-15-001: Lock state propagated through ServiceSnapshot
    require_text(shared_h, "gc_bool8 hasLock", "ServiceSnapshot carries lock state as a fixed-width wire flag")
    require_text(shared_h, "int lockCi", "ServiceSnapshot carries lock curve index")
    require_text(shared_h, "unsigned int lockMHz", "ServiceSnapshot carries lock frequency")
    require_text(shared_h, "gc_bool8 lockTracksAnchor", "ServiceSnapshot carries lock tracking flag as a fixed-width wire flag")
    require_text(main_state_sync_cpp, "adopted service lock ci=", "lock state from snapshot is adopted by GUI")
    require_text(main_state_sync_cpp, "reporting active desired lock", "service snapshots prefer configured lock intent over live tail detection")
    require_text(gpu_backend_cpp, "live lock detection suppressed; preserving intent", "live lock detection does not clear configured lock intent")
    require_text(gpu_backend_cpp, "requestedMHz=", "GUI-side service apply sync preserves requested lock MHz")
    require_text(main_tail_diagnostics_cpp, "curve tail bookends", "telemetry snapshot logs tail bookends to detect post-apply shifts")
    require_text(main_tail_diagnostics_cpp, "service monitor may reapply after confirmation", "runtime tail drift diagnostics describe the gated service reapply monitor")
    require_text(main_tail_diagnostics_cpp, "is_curve_point_visible_in_gui(ci)", "tail drift diagnostics skip hidden/unpopulated VF endpoints")
    require_text(ui_main_cpp, "displayed_curve_mhz_for_gui_point", "GUI graph renders curve points from live driver readback")
    require_text(ui_main_cpp, "gui locked tail live readback drift:", "GUI logs live tail readback drift diagnostics (no longer hidden)")
    require_text(desired_settings_helpers_cpp, "desired_is_fan_only_apply_request", "fan-only apply requests are detected without curve/OC fields")
    require_text(desired_settings_helpers_cpp, "desired_updates_curve_or_gpu_offset_state", "memory/power-only applies do not replace sparse curve intent")
    require_text(main_service_runtime_cpp, "merged fan-only request into active desired", "service fan-only applies preserve active curve intent")
    require_text(gpu_backend_cpp, "skipped VF edit repaint for fan-only apply", "GUI client fan-only applies do not clear sparse curve masks")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "preserving VF editor intent after fan-only apply", "main apply handler preserves VF editor after fan-only apply")
    require_text(main_runtime_control_cpp, "curvePoints=%d (%s)", "GUI capture logs sparse curve point list")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"), "point74=%d/%u point75=%d/%u point76=%d/%u", "profile save logs edited pre-tail VF points")
    require_text(main_runtime_gpu_cpp, "capture_gui_desired_settings(&resetFull, true, true, false", "apply capture keeps sparse VF curve intent")
    require_text(main_runtime_gpu_cpp, "capture_gui_desired_settings(&guiDesired, true, true, false", "profile save capture keeps sparse VF curve intent")
    require_text(config_profile_repair_cpp, "profile repair: removed non-tail readback artifact", "profile load repairs logged non-tail readback artifacts")
    require_text(main_gpu_state_cpp, "skippedLockedTail ? 4 : 3", "selective GPU offset detection rejects two-point high-edit false positives")

    # Lock mode (flatten/pin) must round-trip through profile save. The pin (hard)
    # mode was previously dropped because merge_desired_settings ignored lock state
    # and capture_gui_config_settings forgot lockMode. These would fail before the fix.
    require_text(config_profiles_ui_cpp, "base->lockMode = override->lockMode", "Windows merge_desired_settings carries lock mode")
    require_text(os.path.join(SOURCE_DIR, "linux_port_profiles.cpp"), "base->lockTracksAnchor = incoming->lockTracksAnchor", "Linux merge_desired_settings carries lock anchor tracking")
    require_text(main_runtime_gpu_cpp, "full.lockMode = guiDesired.lockMode", "profile-save capture preserves lock mode from GUI")
    require_text(main_runtime_gpu_cpp, "full.lockMode = g_app.lockMode", "profile-save capture preserves live lock mode")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"), "lock writing ci=", "profile save logs the lock ci/mhz/mode being written")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "show_lock_context_menu", "lock checkbox right-click mode menu exists")
    require_text(os.path.join(SOURCE_DIR, "ui_main.cpp"), "create_lock_tooltips", "lock checkbox hover tooltip is registered")

    # Pin-bug root cause (snapshot lockMode clobber): the per-second telemetry
    # snapshot must never overwrite divergent pending lock intent (a
    # FLATTEN->HARD click or a loaded HARD profile at the same lock point) with
    # the service's previously-applied mode. These would fail before the fix.
    require_text(shared_h, "lock_mode_sync_allowed", "pending-lock-intent gate helper exists in shared header")
    require_text(main_state_sync_cpp, "lock_mode_sync_allowed((int)g_app.lockMode, (int)g_app.appliedLockMode, gui_state_dirty())", "snapshot lockMode sync is gated on no pending lock intent")
    require_text(main_state_sync_cpp, "lockMode sync skipped (pending lock intent", "skipped lockMode sync is logged for diagnosis")
    require_text(gpu_backend_cpp, "g_app.appliedLockMode = g_app.lockMode;", "curve-detection sync keeps the lockMode/appliedLockMode intent invariant")
    require_text(config_profiles_ui_cpp, "desired->hasFan || desired->hasLock", "Windows desired_has_any_action counts a lock-only profile as an action")
    require_text(os.path.join(SOURCE_DIR, "linux_port.cpp"), "desired->hasFan || desired->hasLock", "Linux desired_has_any_action counts a lock-only profile as an action")

    # Persistence hardening: both the service restart snapshot and INI profile
    # loads route through the IPC validator so corrupt bytes cannot reach the
    # apply path or the GUI-side curve math unclamped.
    require_text(os.path.join(SOURCE_DIR, "main_service_persist.cpp"), "validate_desired_settings_for_ipc(&payload.desired);", "restart-reapply snapshot fields are clamped on load")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"), "validate_desired_settings_for_ipc(desired);", "INI profile loads clamp fields before derived curve math")
    require_text(shared_h, "if (d->lockMHz > 5000u)", "IPC validator clamps lockMHz like the curve points")

    # Diagnostics for rare failure modes (no behavior change).
    require_text(config_utils_cpp, "config mutex was abandoned", "abandoned config mutex acquisitions are logged")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "resume reapply CreateThread FAILED", "resume-reapply thread creation failure is logged")

    # GUI repaint robustness during service start/restart (visual corruption).
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "WM_SETREDRAW, FALSE", "edit-control rebuild suppresses painting to avoid partial-paint corruption")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "rebuild_edit_controls", "service-state control rebuild routes through redraw-suppressed helper")
    require_text(service_ipc_cpp, "wait_object_pumping_ui", "blocking service waits pump the GUI so the window keeps repainting")
    require_text(service_ipc_cpp, "MsgWaitForMultipleObjectsEx", "service waits use a message-pumping wait, not a frozen Sleep/WaitForSingleObject")
    require_text(service_ipc_cpp, "struct UiInputGuard", "GUI input is disabled during pumped service waits to block re-entrancy")

    # F-12-001: Backend spec static_assert checks
    # VfBackendSpec tables + their layout static_asserts now live in the shared
    # vf_backends.cpp (compiled on both Windows and Linux).
    require_text(os.path.join(SOURCE_DIR, "vf_backends.cpp"), "static_assert(0x48u + (VF_NUM_POINTS - 1u) * 0x1Cu + 4u <= 0x1C28u", "VF status buffer static_assert exists")
    require_text(os.path.join(SOURCE_DIR, "vf_backends.cpp"), "static_assert(0x04u + 32u <= 0x182Cu", "VF info buffer static_assert exists")
    require_text(os.path.join(SOURCE_DIR, "vf_backends.cpp"), "static_assert(0x44u + (VF_NUM_POINTS - 1u) * 0x24u + 4u <= 0x2420u", "VF control buffer static_assert exists")

    # F-11-001: Service event creation integrity check
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "g_serviceStopEvent) {", "service stop event creation check exists")

    # F-05-001: Rollback retry support
    require_text(os.path.join(SOURCE_DIR, "main_gpu_front.cpp"), "retry_op", "rollback uses retry_op helper")

    # F-07-001: Config int truncation detection
    require_text(config_utils_cpp, "n >= sizeof(buf) - 1", "config int read detects truncation")
    require_text(config_utils_cpp, "errno == ERANGE", "Windows config integer parser rejects C library overflow")
    require_text(os.path.join(SOURCE_DIR, "linux_port.cpp"), "errno == ERANGE", "Linux config integer parser rejects C library overflow")

    # F-LNX: native Linux GPU backend + daemon invariants
    linux_gpu_cpp = os.path.join(SOURCE_DIR, "linux_gpu.cpp")
    linux_backend_cpp = os.path.join(SOURCE_DIR, "linux_backend.cpp")
    linux_daemon_cpp = os.path.join(SOURCE_DIR, "linux_daemon.cpp")
    linux_main_cpp = os.path.join(SOURCE_DIR, "linux_main.cpp")
    vf_backends_cpp = os.path.join(SOURCE_DIR, "vf_backends.cpp")
    # The old --apply-config blocker must be gone (Linux apply is implemented).
    forbid_text(linux_main_cpp, "intentionally blocked until native Linux VF-curve parity",
                "Linux --apply-config blocker has been removed (apply implemented)")
    # NvAPI on Linux: load the proprietary driver's private library + use the
    # negative-error-aware OK test (Linux NvAPI errors are negative, not 0x8000xxx).
    require_text(linux_backend_cpp, "status == 0", "Linux nvapi_ok uses status==0 (Linux NvAPI errors are negative)")
    require_text(linux_gpu_cpp, "libnvidia-api.so", "Linux probe loads NvAPI via libnvidia-api.so")
    require_text(os.path.join(SOURCE_DIR, "platform.h"), "libnvidia-ml.so.1", "platform shim knows the NVML soname")
    # Single-bit-mask VF point writes (driver rejects multi-bit masks on Linux).
    require_text(linux_backend_cpp, "writeMask[i / 8] |= (unsigned char)(1u << (i % 8))",
                 "Linux VF write builds a per-point control mask")
    require_text(linux_backend_cpp, "apply_curve_offsets_verified",
                 "Linux backend ports the verified VF-curve correction loop")
    # Daemon: IPC validation at the trust boundary + peer-cred audit.
    require_text(linux_daemon_cpp, "validate_desired_settings_for_ipc",
                 "Linux daemon clamps requests at the IPC trust boundary")
    require_text(linux_daemon_cpp, "SO_PEERCRED", "Linux daemon logs peer credentials")
    require_text(linux_daemon_cpp, "persist_active_desired",
                 "Linux daemon persists active settings for restart-reapply")
    require_text(linux_daemon_cpp, "startup reapply", "Linux daemon reapplies settings on (re)start")
    require_text(linux_daemon_cpp, "GC_DAEMON_IO_TIMEOUT_MS", "Linux daemon socket I/O has bounded deadlines")
    require_text(linux_daemon_cpp, "wait_fd_ready", "Linux daemon socket reads/writes poll with a deadline")
    require_text(linux_daemon_cpp, "set_nonblocking(conn)", "Linux daemon accepted clients are nonblocking")
    require_text(linux_daemon_cpp, "unlink(GC_DAEMON_STATE_FILE)", "Linux reset deletes stale active.bin")
    require_text(linux_daemon_cpp, "GC_INSTALL_BIN", "Linux systemd unit uses a protected staged daemon binary")
    require_text(linux_daemon_cpp, "root_owned_nonwritable_path", "Linux service install validates root-owned non-writable parents")
    require_text(linux_daemon_cpp, "stage_service_binary", "Linux service install stages the daemon binary before writing the unit")
    require_text(linux_daemon_cpp, "ExecStart=%s --daemon", "Linux systemd unit launches the staged daemon path")
    # F-LNX-TUI: root-cause fix for the reported mouse-offset bug + keyboard/daemon parity.
    linux_tui_cpp = os.path.join(SOURCE_DIR, "linux_tui.cpp")
    linux_tui_layout_cpp = os.path.join(SOURCE_DIR, "linux_tui_layout.cpp")
    require_text(linux_tui_layout_cpp, "(int)out->lines.size() + 1",
                 "TUI button row is derived from the emitted line count (cannot drift)")
    require_text(linux_tui_layout_cpp, "tui_display_columns",
                 "TUI hitbox X uses UTF-8 display columns, not byte length")
    forbid_text(linux_tui_cpp, "int y = 1;",
                "TUI no longer hand-tracks a row counter that drifts from the printed rows")
    require_text(linux_tui_cpp, "build_tui_layout",
                 "TUI renders and hit-tests from the shared pure layout builder")
    require_text(linux_tui_cpp, "button & 64",
                 "TUI ignores wheel-scroll mouse reports (no phantom clicks)")
    require_text(linux_tui_cpp, "(button & 3) != 0",
                 "TUI mouse activation accepts only the left button")
    require_text(linux_tui_cpp, "focus_step_vertical",
                 "TUI has keyboard navigation (arrow/Tab focus model)")
    require_text(linux_tui_cpp, "linux_daemon_apply",
                 "TUI can apply settings live to the GPU via the daemon")
    # F-ARM64: cross-arch robustness (no arm64 hardware to test on).
    gpu_core_h_path = os.path.join(SOURCE_DIR, "gpu_core.h")
    require_text(gpu_core_h_path, "offsetof(nvapiPstate20Entry_t, clocks) == 8",
                 "NVAPI struct field offsets pinned at compile time (arm64 layout proof)")
    require_text(gpu_core_h_path, "__ORDER_LITTLE_ENDIAN__",
                 "gpu_core.h asserts a little-endian target")
    require_text(linux_backend_cpp, "linux_backend_curve_plausible",
                 "VF curve read is sanity-checked for plausibility")
    require_text(linux_backend_cpp, "REFUSING VF curve write",
                 "VF writes are gated off when the read looks implausible (fail-safe)")
    require_text(linux_backend_cpp, "INCOMPATIBLE_STRUCT_VERSION",
                 "NvAPI negative-error names mapped for diagnostics")
    require_text(linux_backend_cpp, "linux_backend_self_test",
                 "read-only driver/arch self-test exists")
    require_text(linux_gpu_cpp, "nv_tegra_release",
                 "probe detects unsupported Tegra/Jetson platform")
    require_text(build_script, "def inspect_aarch64_driver",
                 "build.py can inspect an aarch64 driver tree for required libs/symbols")
    # Shared VfBackendSpec tables compiled on both platforms (one copy).
    require_text(vf_backends_cpp, "g_vfBackendBlackwell", "shared VfBackendSpec tables define the Blackwell backend")
    # glibc-dynamic Linux target (static musl can't dlopen the glibc driver libs).
    require_text(build_script, '"x86_64-linux-gnu"', "Linux target is glibc-dynamic (x86_64-linux-gnu)")
    # Multi-arch build matrix + release archiving.
    require_text(build_script, 'default="all"', "default build covers all OSes (windows + linux)")
    require_text(build_script, '"-target", "aarch64-windows-gnu"', "windows arm64 Zig build target triple defined")
    require_text(build_script, 'LINUX_ARM64_TRIPLE = "aarch64-linux-gnu"', "linux arm64 target triple defined")
    require_text(build_script, "-mbranch-protection=standard", "windows arm64 uses BTI/PAC instead of x86 CET")
    require_text(build_script, "def package_release_archive", "release archives are produced by build.py")
    require_text(build_script, 'f"greencurve-{APP_VERSION}-{os_name}-{arch}.7z"', "archive name carries version + os + arch")
    require_text(build_script, '"a", "-t7z"', "release files are packaged with 7-Zip")
    require_text(build_script, "def detect_binary_arch", "packaging reads each binary's real PE/ELF machine arch")
    require_text(build_script, "architecture mismatch packaging", "packaging aborts on a cross-arch bundle mismatch")
    require_text(build_script, "-mbranch-protection=standard", "arm64 builds enable BTI/PAC branch protection")
    # ARM64 VEH thread-redirect uses the aarch64 register names.
    require_text(os.path.join(SOURCE_DIR, "main_diagnostics.cpp"), "ContextRecord->Pc",
                 "VEH thread-exit redirect supports arm64 (Pc/X0/Sp)")

    require_text(fan_curve_cpp, "fan_curve_set_default(config)", "invalid fan curve normalization resets safely to defaults")
    require_text(shared_h, "len > bufSize - offset", "HeapBuffer bounds checks avoid size_t addition overflow")

    # F-01-004: ASan flag exists
    require_text(build_script, "--asan", "ASan build flag exists")
    require_text(build_script, "llvm_bin = os.path.dirname(LLVM_MINGW_CLANG)", "ASan test runner can find llvm-mingw sanitizer runtime")

    # F-10-001: Config mutex timeout diagnostic
    require_text(os.path.join(SOURCE_DIR, "config_utils.cpp"), "config mutex timed out", "config mutex timeout warning exists")

    # FP-01-001: Power event registration for resume detection
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "RegisterServiceCtrlHandlerExW", "service uses Ex control handler for power events")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "SERVICE_ACCEPT_POWEREVENT", "service accepts power events")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "PBT_APMRESUMEAUTOMATIC", "service handles resume from standby")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "service_check_oc_persistence", "resume handler checks OC persistence")
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"), "service_resume_reapply_thread_proc", "resume handler uses dedicated thread")

    # FP-01-002: TDR loop detection for driver update / OC crash protection
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "RECOVERY_LOOP_WINDOW_MS", "TDR loop detection window exists")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "MAX_RECOVERIES_BEFORE_BACKOFF", "TDR loop backoff threshold exists")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "oc_persistence: loop detected", "TDR loop detection logs warning")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "oc_persistence: checking", "OC persistence logs trigger source (driver recovery vs standby resume)")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "oc_persistence: all settings intact", "OC persistence check verifies all settings before re-applying")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "oc_persistence: settings lost:", "OC persistence re-applies with reset when settings lost")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "GPU offset live=", "OC persistence check reads GPU offset")
    require_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "re-applying with reset", "OC persistence uses reset-before-apply on auto re-apply (standby/resume path)")

    # FP-01-003: GPU-driver-restart recovery must not use the old ad-hoc fan
    # pulse NVML re-init path (which crash-looped/hung).  See FP-06 for the
    # restart-based recovery; assert the old in-process re-init path is gone.
    forbid_text(os.path.join(SOURCE_DIR, "main_service_runtime.cpp"), "NVML stale, attempting recovery",
        "fan pulse no longer does ad-hoc NVML recovery")

    # FP-01-005: Increased VF offset range limit for tail flatten
    require_text(gpu_backend_apply_cpp, "FALLBACK_VF_OFFSET_LIMIT_KHZ = 500000", "VF offset fallback increased to 500 MHz for tail flatten")
    require_text(gpu_backend_apply_cpp, "tail point %d stuck at actual=%u target=%u", "tail point stuck diagnostic logging exists")
    require_text(gpu_backend_apply_cpp, "tail point %d out of range", "tail point out-of-range diagnostic logging exists")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_nvml.cpp"), "FALLBACK_VF_OFFSET_LIMIT_KHZ = 1000000", "VF offset range fallback uses GPU offset range (1000 MHz)")

    # FP-02-001: Uniform tail floor offset (Blackwell per-point delta fix)
    require_text(gpu_backend_apply_cpp, "floorTailOffsetKHz", "uniform tail floor offset constant exists for initial tail loop")
    require_text(gpu_backend_apply_cpp, "correctionFloorTailOffsetKHz", "uniform tail floor offset constant exists for correction passes")
    require_text(gpu_backend_apply_cpp, "tail uniform floor offset=%d", "correction pass logs uniform tail floor offset write")
    require_text(gpu_backend_apply_cpp, "Determine the uniform floor offset for tail points.", "initial tail loop uses uniform floor for non-lock tail points")

    # FP-02-002: Pre-tail point capture after restart (non-zero offset detection, guarded by profile load check)
    require_text(os.path.join(SOURCE_DIR, "main_runtime_control.cpp"), "preTailInferred", "pre-tail user-modified points inferred from non-zero live offset (guarded by hasPreTailExplicit)")

    # FP-02-003: Stale NVML memory VF offset cleared on fresh service start
    require_text(os.path.join(SOURCE_DIR, "main_state_sync.cpp"),
        "stale mem VF offset %d kHz detected",
        "stale NVML mem VF offset is detected and cleared on fresh service start")

    # FP-03-001: nvapi_qi() module-level cache invalidated by close_nvapi()
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "g_nvapiQi = nullptr",
        "nvapi_qi() module-level cache is reset on close_nvapi() to prevent stale function pointer crashes")

    # FP-03-002: close_nvapi() clears hardware-init guards for full re-init
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "g_app.loaded = false",
        "close_nvapi() clears loaded flag so hardware_initialize() fully re-enumerates GPUs")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "g_app.vfBackend = nullptr",
        "close_nvapi() clears VF backend so it is re-selected on next hardware_initialize()")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "g_app.gpuHandle = nullptr",
        "close_nvapi() clears GPU handle so it is re-acquired on next hardware_initialize()")

    # FP-03-003: close_nvapi() still resets all NVAPI state (module-level cache,
    # adapter cache, hardware-init guards) for the normal GUI/shutdown close path.
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "g_nvapiQi = nullptr",
        "close_nvapi() still invalidates the nvapi_qi() module-level cache")

    # FP-03-004: Device arrival handler reads deviceWasRemoved before clearing the flag
    require_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"),
        "bool deviceWasRemoved = g_app.deviceRemoved;",
        "device arrival handler saves removal flag before clearing it")

    # FP-03-005: Device arrival no longer re-inits NVML/NVAPI directly on the
    # SCM control thread; it requests a service restart via the chokepoint (FP-06).
    forbid_text(os.path.join(SOURCE_DIR, "main_service_server.cpp"),
        "device event: NVML re-initialized after device arrival",
        "device arrival handler no longer re-inits NVML on the SCM control thread")

    # FP-04-001: hardware_initialize() skips refresh_global_state() during NVML
    # crash recovery keyed on g_nvmlCrashCount.  The earlier check was keyed on
    # `g_app.gpuHandle == nullptr`, but nvapi_enum_gpu() always sets gpuHandle
    # non-null first, so the skip was dead code and refresh_global_state() ran
    # on every recovery — the NVML access-violation the crash loop kept hitting.
    require_text(main_state_sync_cpp,
        "skipping global state refresh during NVML crash recovery",
        "hardware_initialize() skips refresh_global_state() during NVML crash recovery")
    require_text(main_state_sync_cpp,
        "if (nvml_crash_recovery_active()) {",
        "hardware_initialize() global-state-refresh skip uses the recovery-window helper, not the dead gpuHandle check")
    forbid_text(main_state_sync_cpp,
        "bool wasFreshInit = (g_app.gpuHandle == nullptr)",
        "dead wasFreshInit==gpuHandle skip removed from hardware_initialize() (always-false, never skipped)")

    # FP-04-004: shared NVML crash recovery window helper used by the pipe
    # server snapshot handler and the telemetry handler so GUI requests serve
    # cached globals (instead of access-violating in refresh_global_state)
    # while the fan runtime thread drives recovery.
    require_text(main_cpp,
        "static bool nvml_crash_recovery_active()",
        "shared NVML crash recovery window helper exists")
    require_text(service_server_cpp,
        "nvml_crash_recovery_active()",
        "snapshot handler uses the recovery-window helper to serve cached globals")
    forbid_text(service_server_cpp,
        "if (InterlockedExchange(&g_nvmlVhCrashed, 0)) {",
        "snapshot handler no longer consumes g_nvmlVhCrashed (left intact for fan thread recovery)")

    # FP-06: GPU driver-recovery via SERVICE-PROCESS RESTART.
    # In-process reload of nvml.dll / nvapi64.dll after a GPU device reconnect or
    # an in-place driver upgrade is unreliable: the NVIDIA user-mode DLLs stay
    # mapped (driver-pinned), so FreeLibrary does not unmap them and the new
    # on-disk DLL is never loaded; nvmlInit then returns ALREADY_INITIALIZED on a
    # handle bound to the dead driver instance, and NvAPI's process-global UMD
    # cannot be reloaded and must version-match the kernel driver.  Recovery is
    # therefore performed by restarting the service PROCESS: snapshot the active
    # profile, exit non-zero, let the SCM failure action relaunch us, and re-apply
    # the snapshot on the fresh (clean-DLL) process.

    # FP-06-001: the broken in-process reload machinery is gone.
    forbid_text(main_service_runtime_cpp,
        "service_recover_gpu_connection",
        "in-process GPU recovery (Phase A-E reload) has been removed")
    forbid_text(main_service_runtime_cpp,
        "service_recovery_thread_proc",
        "in-process recovery thread has been removed")
    forbid_text(main_service_runtime_cpp,
        "service_safe_close_nvml(",
        "in-process NVML close-for-reload has been removed")
    forbid_text(main_service_runtime_cpp,
        "service_safe_close_nvapi(",
        "in-process NvAPI close-for-reload has been removed")

    # FP-06-002: every recovery trigger routes through the single chokepoint
    # launch_recovery_thread(), which now only requests a controlled restart.
    require_text(main_service_runtime_cpp,
        "static void launch_recovery_thread() {",
        "launch_recovery_thread is the single recovery chokepoint")
    require_text(main_service_runtime_cpp,
        "request_service_restart(\"GPU driver recovery",
        "launch_recovery_thread requests a service-process restart")
    require_text(service_server_cpp,
        "launch_recovery_thread();",
        "DBT_DEVICEARRIVAL routes to the restart chokepoint")
    require_text(service_server_cpp,
        "request_service_restart(\"fan pulse wedged",
        "fan-pulse wedge watchdog requests a service restart")

    # FP-06-003: request_service_restart snapshots + records the restart, and the
    # restart-exit is SAFE (no NVML teardown that can hang) and terminates WITHOUT
    # reporting SERVICE_STOPPED so the SCM's DEFAULT crash-recovery path fires the
    # SC_ACTION_RESTART failure action (no non-crash flag / admin rights needed —
    # the LocalSystem service token cannot set that flag at runtime).
    require_text(main_cpp,
        "static void request_service_restart(const char* reason) {",
        "request_service_restart exists")
    require_text(main_cpp,
        "service_record_restart_event();",
        "request_service_restart records the restart for loop protection")
    require_text(service_server_cpp,
        "InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0",
        "service_main has a driver-recovery restart-exit branch")
    require_text(service_server_cpp,
        "terminating WITHOUT reporting SERVICE_STOPPED",
        "restart exit terminates without reporting SERVICE_STOPPED (SCM default failure-action path)")
    # Regression guard: the recovery exit must NOT re-introduce a reported
    # SERVICE_STOPPED with a service-specific exit code (that needs the non-crash
    # flag the LocalSystem service cannot set, so the SCM never relaunches).
    forbid_text(service_server_cpp,
        "ERROR_SERVICE_SPECIFIC_ERROR",
        "recovery restart must not report SERVICE_STOPPED with a service-specific exit code")
    require_text(service_server_cpp,
        "ExitProcess(1);",
        "restart exit terminates the process so the SCM relaunches a fresh one")
    # The restart-exit branch must run BEFORE the normal shutdown NVML teardown
    # (service_reset_all / close_nvml can HANG on a dead/transitional driver).
    require_order(service_server_cpp,
        "ExitProcess(1);",
        "service_reset_all(resetDetail",
        "restart exit (ExitProcess) precedes the normal NVML teardown")

    # FP-06-004: SCM auto-restart failure actions are configured at install AND at
    # every service start (so installs predating this code still auto-restart).
    require_text(main_service_install_cpp,
        "SC_ACTION_RESTART",
        "service configures SCM auto-restart failure actions")
    require_text(main_service_install_cpp,
        "ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS",
        "service_configure_failure_actions applies SERVICE_CONFIG_FAILURE_ACTIONS")
    require_text(service_server_cpp,
        "service_ensure_failure_actions_configured();",
        "service_main re-applies SCM failure actions at startup")
    # F-REL-1: service_main verifies (queries) the SCM auto-restart net at startup
    # and logs loudly when it is NOT ARMED (LocalSystem cannot set it at runtime).
    require_text(service_server_cpp, "service_verify_restart_safety_net()",
        "service_main verifies the SCM auto-restart safety net at startup")
    require_text(main_service_install_cpp, "auto-restart net is %s",
        "restart-safety verification logs ARMED/NOT ARMED state")
    # F-SEC-3: the pipe ACL is user-agnostic (authenticated local users
    # read+write); active-session enforcement is server-side (see F-15-012).  A
    # per-user pipe ACL would lock the next active user out of connecting after
    # an account switch until reboot, so it is intentionally NOT used.
    require_text(service_server_cpp, "(A;;GRGW;;;AU)",
        "pipe ACL grants Authenticated Users read+write (active-session enforced server-side)")

    # FP-06-005 / F-BUG-016/F-BUG-017: a fresh process restores a controlled
    # recovery profile via the startup coordinator, with persisted restart-loop
    # protection so a broken driver cannot loop.  A no-snapshot service start is
    # deliberately idle: it must not race independent reset/apply workers and
    # must not treat an already logged-in user as a fresh logon.
    require_text(main_service_recovery_cpp,
        "service_startup_coordinator_thread_proc",
        "startup coordinator restores restart snapshots on a fresh process")
    require_text(service_server_cpp,
        "service_launch_startup_coordinator();",
        "service_main launches one serialized startup coordinator")
    require_text(main_service_persist_cpp,
        "service_write_restart_reapply_snapshot",
        "restart reapply snapshot writer exists")
    require_text(main_service_recovery_cpp,
        "service_choose_recovery_reapply_desired",
        "reapply chooses active desired before disk snapshot fallback")
    require_text(main_service_persist_cpp,
        "service_record_restart_event",
        "restart events are recorded for loop protection")
    require_text(main_service_recovery_cpp,
        "service_count_recent_restarts",
        "startup reapply checks the persisted restart-loop counter")
    require_text(main_service_recovery_cpp,
        "RESTART LOOP detected",
        "startup reapply breaks an infinite restart loop")
    # F-REL-2: the breaker goes dormant and RETAINS the snapshot (does not discard
    # the user's profile); a genuine device arrival re-arms reapply; minidumps are
    # rotated so a restart loop cannot fill the disk.
    require_text(main_service_recovery_cpp, "going DORMANT",
        "restart-loop breaker goes dormant and retains the snapshot")
    # F-REL-2b: if the driver is not ready within the startup readiness wait
    # (e.g. a prolonged Device Manager disable), the startup reapply does NOT
    # abandon the profile — it hands off to the main-loop reapply worker (after
    # promoting the disk snapshot to the in-memory active desired) so the running
    # service reapplies once the driver finishes initializing.
    require_text(main_service_recovery_cpp, "main-loop reapply worker (snapshot retained)",
        "startup reapply defers to the worker instead of abandoning when the driver is slow to init")
    require_order(main_service_recovery_cpp,
        "g_serviceHasActiveDesired = true;",
        "service_queue_recovery_reapply(\"startup reapply: driver not ready",
        "startup reapply promotes the snapshot to active desired before queuing the worker")
    # F-REL-2c: the reapply worker waits for a driver INDEFINITELY — "driver not
    # ready yet" is retried without consuming the bounded attempt budget, so the
    # configured OC/fan are guaranteed to reapply once any driver becomes active.
    # The bound stays only for the dangerous "driver ready but apply fails" case.
    require_text(main_service_recovery_cpp, "NOT counted against the retry budget",
        "reapply worker waits for a driver indefinitely (no time cap on driver readiness)")
    # F-REL-2d / F-BUG-017: a startup WITHOUT a pending restart snapshot is not
    # a proof that Green Curve owns the hardware state.  Installing, repairing,
    # or manually starting the service (--manual) while a user is already logged
    # in must NOT apply that user's logon slot behind the user's back.
    # Recovery restarts still use the persisted restart snapshot path above, and
    # an unexpected termination (Task Manager kill) may leave stale OC settings on
    # the GPU; detect and reset them on no-snapshot startup.  0.17.1: a genuine
    # BOOT auto-start (no --manual) with an already-active session DOES reconcile
    # that session's logon profile once (Fast Startup / autologon safety net); the
    # boot-vs-manual + at-most-once-per-boot gating is enforced by the pure,
    # unit-tested should_reconcile_active_session_at_boot() (checked above) so this
    # cannot regress into a manual-start mutation.
    require_text(main_service_recovery_cpp,
        "no restart snapshot; checking for stale GPU OC settings",
        "no-snapshot startup checks for stale GPU OC settings")
    require_text(main_service_recovery_cpp,
        "no stale GPU OC settings found",
        "no-snapshot startup logs when no stale OC settings exist")
    require_text(main_service_recovery_cpp, "final activeDesired=",
        "startup coordinator logs final active desired state")
    # F-REL-2e: after the service authoritatively resets to no-lock, the GUI clears
    # a stale ADOPTED lock (checkbox / point value / header) when the user is not
    # dirty-editing — otherwise the gate would pin it forever once lockedCi>=0.
    require_text(state_sync_cpp, "clearing stale adopted GUI lock",
        "GUI clears a stale adopted lock when the service reports authoritative no-lock and the user is not editing")
    require_text(main_fan_runtime_cpp, "g_guiForceFullRefresh",
        "telemetry poll does a full GUI resync (graph/fields/checkboxes/header) when a reset clears a stale lock")
    # F-REL-2f: while minimized to the tray, keep a slow telemetry poll so the tray
    # icon/tooltip reflect service state changes (e.g. a reset) without opening the window.
    require_text(main_gpu_front_cpp, "TRAY_HIDDEN_POLL_INTERVAL_MS",
        "tray icon keeps a slow telemetry poll while the window is hidden")
    # F-REL-2g: the tray icon AND tooltip only report OC/fan/profile as active when
    # the GPU live state is actually available (tray_hardware_live()), so a disabled
    # driver / down service shows the default icon and a "GPU driver unavailable"
    # tooltip instead of a false active state for the merely-pending desired.
    require_text(main_gpu_front_cpp, "static bool tray_hardware_live()",
        "shared tray hardware-availability helper exists")
    require_text(main_fan_runtime_cpp, "tray_hardware_live()",
        "tray icon gates OC/fan-active on actual GPU availability (not a pending desired)")
    require_text(main_gpu_front_cpp, "Green Curve - GPU driver unavailable",
        "tray tooltip reports GPU unavailable instead of a false OC/fan/profile-active state")
    # F-BUG-016b: runtime VF drift is no longer diagnostic-only.  The service
    # checks active desired VF intent on the existing watchdog cadence, requires
    # consecutive confirmed drift, caps/backs off queueing, and queues the
    # existing recovery reapply worker instead of writing from the monitor path.
    require_text(main_cpp, "SERVICE_VF_DRIFT_CONFIRM_SAMPLES = 2",
        "VF drift monitor requires consecutive confirmed samples")
    require_text(main_cpp, "SERVICE_VF_DRIFT_MAX_QUEUES_PER_WINDOW = 3",
        "VF drift monitor caps repeated reapply queues")
    require_text(main_service_vf_drift_cpp, "service_active_vf_intent_drifted",
        "VF drift monitor compares live VF state to active desired intent")
    require_text(main_service_vf_drift_cpp, "service_queue_recovery_reapply(\"VF drift monitor\", 0)",
        "VF drift monitor queues the existing reapply worker")
    require_text(main_shell_cpp, "#ifdef GREEN_CURVE_SERVICE_BINARY\n#include \"main_service_vf_drift.cpp\"",
        "service VF drift monitor shard is included only in the service binary")
    require_text(service_server_cpp, "service_check_active_vf_drift_monitor(\"main loop\")",
        "service main loop invokes the VF drift monitor")
    require_text(main_tail_diagnostics_cpp, "service monitor may reapply after confirmation",
        "tail drift diagnostic text reflects the service reapply monitor")
    require_text(main_tail_diagnostics_cpp, "s_tailDriftLastLoggedValid",
        "tail drift diagnostics log first/change/reappeared drift instead of every telemetry poll")
    # F-REL-4: OC stabilization window — settings that crash-restart the service
    # within 10 min of being applied are treated as unstable and NOT auto-reapplied
    # (by either reapply method), so an unstable OC cannot loop.  Stamp recorded on
    # the user-initiated apply; checked + dropped by the restart-based reapply (the
    # clear there neutralizes the in-process resume path too).
    require_text(main_service_persist_cpp, "SERVICE_OC_STABILIZATION_WINDOW_MS",
        "OC stabilization window constant exists")
    require_text(main_service_persist_cpp, "service_oc_within_stabilization_window",
        "OC stabilization window helper exists")
    require_text(service_server_cpp, "service_record_oc_apply_stamp()",
        "user-initiated apply records the OC stabilization stamp")
    require_text(service_server_cpp, "service_clear_oc_apply_stamp()",
        "reset clears the OC stabilization stamp")
    require_text(main_service_recovery_cpp, "service_oc_within_stabilization_window()",
        "startup reapply drops settings that crashed within the OC stabilization window")
    require_text(service_server_cpp, "service_clear_restart_history();",
        "device arrival re-arms reapply by clearing the restart-loop history")
    require_text(state_sync_cpp, "service_rotate_minidumps",
        "VEH minidumps are rotated to bound disk usage")
    require_text(service_server_cpp, "service_rotate_minidumps(",
        "service startup rotates VEH minidumps")

    # FP-06-006: the VEH is the crash DETECTOR only — it invalidates NVML without
    # nvmlShutdown and lets the main loop request the restart.
    require_text(main_service_runtime_cpp,
        "service_close_nvml_without_shutdown",
        "driver-crash recovery has a no-shutdown NVML invalidation helper")
    require_text(diagnostics_cpp,
        "service_close_nvml_without_shutdown();",
        "VEH crash path invalidates NVML without nvmlShutdown")
    forbid_text(diagnostics_cpp,
        "close_nvml();",
        "VEH crash path must not call nvmlShutdown via close_nvml")
    require_text(runtime_nvml_cpp,
        "g_nvmlVhCrashed || nvml_crash_recovery_active()",
        "nvml_ensure_ready() reports not-ready during the brief pre-restart crash window")

    # FP-06-007: on-disk driver-version detection is retained (logged at the
    # recovery trigger so a driver upgrade is correlated in the debug log).
    require_text(main_service_runtime_cpp,
        "service_nvml_disk_version_changed",
        "on-disk NVML version-change detector exists")
    require_text(main_service_runtime_cpp,
        "service_nvapi_disk_version_changed",
        "on-disk NvAPI version-change detector exists")
    require_text(service_server_cpp,
        "service_check_disk_version_on_device_arrival",
        "DBT_DEVICEARRIVAL logs the on-disk driver version delta")
    require_text(build_py_text,
        '"-lversion"',
        "version.lib linked for GetFileVersionInfoW / VerQueryValueW")

    # FP-06-008: the SCM auto-restart depends on opting into non-crash failure
    # actions.  The recovery exit reports SERVICE_STOPPED with a non-zero exit
    # code; without SERVICE_CONFIG_FAILURE_ACTIONS_FLAG /
    # fFailureActionsOnNonCrashFailures=TRUE the SCM treats that as a graceful
    # stop and never relaunches us, so the service stays dead after a GPU
    # reconnect / driver upgrade.
    require_text(main_service_install_cpp,
        "SERVICE_CONFIG_FAILURE_ACTIONS_FLAG",
        "service opts into non-crash failure actions so a reported SERVICE_STOPPED restarts")
    require_text(main_service_install_cpp,
        "fFailureActionsOnNonCrashFailures = TRUE",
        "non-crash-failure flag is set TRUE so non-zero-exit STOPPED triggers SC_ACTION_RESTART")
    require_order(main_service_install_cpp,
        "SERVICE_CONFIG_FAILURE_ACTIONS,",
        "SERVICE_CONFIG_FAILURE_ACTIONS_FLAG",
        "failure actions are configured before the non-crash-failure flag in the same helper")

    # FP-05-002: APPLY/RESET command handlers reject requests during the NVML
    # crash recovery window instead of running NVML/NVAPI writes that crash the
    # pipe server thread (GUI then sees ERROR_BROKEN_PIPE / error 109).
    require_text(service_server_cpp,
        "service APPLY rejected: NVML crash recovery in progress",
        "APPLY handler rejects during NVML crash recovery window")
    require_text(service_server_cpp,
        "service RESET rejected: NVML crash recovery in progress",
        "RESET handler rejects during NVML crash recovery window")

    # FP-05-003: race-free pipe handle ownership so a double-close during the
    # pipe-thread kill/recreate storm can't hard-crash the process via
    # STATUS_INVALID_HANDLE under Strict Handle Checks.
    require_text(service_server_cpp,
        "static void service_close_owned_pipe(HANDLE pipe)",
        "pipe server uses race-free single-owner close helper")
    require_text(service_server_cpp,
        "InterlockedCompareExchangePointer",
        "pipe close helper atomically claims handle ownership before closing")



def parse_args():
    parser = argparse.ArgumentParser(description="Build Green Curve targets with Zig")
    parser.add_argument(
        "--target",
        choices=("windows", "linux", "all"),
        default="all",
        help="Which OS to build (default: all = windows + linux)",
    )
    parser.add_argument(
        "--arch",
        choices=("x64", "arm64", "all"),
        default="all",
        help="Which architecture(s) to build (default: all = x64 + arm64)",
    )
    parser.add_argument(
        "--no-package",
        action="store_true",
        help="Skip building the per-target 7-Zip release archives",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Build selected targets into a temporary directory and generate LSP metadata",
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="Run pure regression tests that do not touch GPU hardware",
    )
    parser.add_argument(
        "--lsp",
        action="store_true",
        help="Generate compile_commands.json for clangd and exit",
    )
    parser.add_argument(
        "--sanitizer",
        action="store_true",
        help="Build with UndefinedBehaviorSanitizer (now default in --test)",
    )
    parser.add_argument(
        "--asan",
        action="store_true",
        help="Build with AddressSanitizer in addition to UBSan",
    )
    parser.add_argument(
        "--inspect-aarch64-driver",
        metavar="DIR",
        default=None,
        help="Verify (no arm64 hardware needed) that an extracted aarch64 NVIDIA "
             "driver dir ships libnvidia-ml.so / libnvidia-api.so and our symbols",
    )
    return parser.parse_args()


def generate_version_nsh():
    _VERSION_NSH_PATH = os.path.join(SCRIPT_DIR, "version.nsh")
    with open(_VERSION_NSH_PATH, "w", encoding="utf-8") as _vnf:
        _vnf.write(f'!define VERSION "{APP_VERSION}"\n')


def ensure_toolchain(target):
    """Download and verify the toolchain(s) needed for the given target."""
    needs_windows = target in ("windows", "all")
    needs_linux = target in ("linux", "all")
    if needs_windows:
        download_llvm_mingw()
    if needs_linux:
        download_zig()


def main():
    args = parse_args()
    if args.inspect_aarch64_driver:
        sys.exit(inspect_aarch64_driver(args.inspect_aarch64_driver))
    print("=== Green Curve build ===")
    ensure_toolchain(args.target)
    real_build = not args.lsp and not args.check and not args.test
    configure_build_number(real_build)
    generate_version_nsh()

    # H-001 fix: avoid permanently mutating the global COMMON_FLAGS list.
    # Save the original flags so sanitizer additions do not leak into
    # subsequent release builds in the same process.
    _original_common_flags = COMMON_FLAGS.copy()
    if args.sanitizer:
        COMMON_FLAGS.extend(SANITIZER_FLAGS)
    try:
        if args.lsp:
            generate_lsp_files()
            print("=== Done ===")
            return
        if args.test:
            # Always run with UBSan by default (F-01-001).
            # --sanitizer is accepted for backward compatibility but is now the default behavior.
            test_extra_flags = list(SANITIZER_FLAGS)
            if args.asan:
                test_extra_flags.append("-fsanitize=address")
                test_extra_flags.append("-g")
            run_regression_tests(extra_flags=test_extra_flags)
            if not args.check:
                print("=== Done ===")
                return
        if args.check:
            run_check_builds(args.target, generate_lsp=not args.sanitizer)
            print("=== Done ===")
            return
        generate_lsp_files()
        oses = ["windows", "linux"] if args.target == "all" else [args.target]
        arches = ["x64", "arm64"] if args.arch == "all" else [args.arch]
        built = []  # (os_name, arch, [binary_path, ...])

        def fresh_payload(os_name, arch):
            # Wipe + recreate the target's isolated folder so each build is clean
            # and no two targets ever share an output or temp path.
            payload = target_payload_dir(os_name, arch)
            shutil.rmtree(os.path.dirname(payload), ignore_errors=True)
            os.makedirs(payload, exist_ok=True)
            return payload

        for arch in arches:
            if "windows" in oses:
                payload = fresh_payload("windows", arch)
                gui = os.path.join(payload, "greencurve.exe")
                svc = os.path.join(payload, "greencurve-service.exe")
                compile_windows_binary(output_path=gui, temp_output=gui + ".new", backup_path=gui + ".bak", arch=arch)
                compile_windows_service_binary(output_path=svc, temp_output=svc + ".new", backup_path=svc + ".bak", arch=arch)
                built.append(("windows", arch, [gui, svc]))
            if "linux" in oses:
                payload = fresh_payload("linux", arch)
                out = os.path.join(payload, "greencurve")
                compile_linux_binary(output_path=out, temp_output=out + ".new", backup_path=out + ".bak", arch=arch)
                built.append(("linux", arch, [out]))
        if not args.no_package and built:
            print("--- Packaging release archives ---")
            for os_name, arch, binaries in built:
                package_release_archive(os_name, arch, binaries)
        print("=== Done ===")
    finally:
        COMMON_FLAGS[:] = _original_common_flags


if __name__ == "__main__":
    main()
