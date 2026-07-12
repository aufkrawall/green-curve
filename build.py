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
import re
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
    os.path.join(SOURCE_DIR, "linux_daemon_state.cpp"),
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
APP_BUILD_NUMBER = 0
_version_path = os.path.join(SCRIPT_DIR, "VERSION")
try:
    with open(_version_path, "r", encoding="utf-8") as _vf:
        APP_VERSION = _vf.read().strip()
except OSError as exc:
    raise SystemExit(f"VERSION is required and could not be read: {exc}")
if not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", APP_VERSION):
    raise SystemExit(f"VERSION must be numeric MAJOR.MINOR[.PATCH], got {APP_VERSION!r}")
COMMON_FLAGS.append(f'-DAPP_VERSION="{APP_VERSION}"')

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
    "-lsetupapi",
    "-lcfgmgr32",
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
    "-lsetupapi",
    "-lcfgmgr32",
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


def windows_symbol_output_path(output_path, arch):
    """Keep private matching symbols outside release payloads/archives."""
    name = os.path.basename(output_path)
    if name.endswith(".new"):
        name = name[:-4]
    stem = os.path.splitext(name)[0]
    extension = ".pdb" if arch == "x64" else ".debug"
    return os.path.join(DIST_DIR, "symbols", f"windows-{arch}", stem + extension)


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


def get_windows_gui_compile_command(temp_output, arch="x64", pdb_path=None):
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
            "-fno-lto",
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
        *(["-gcodeview", f"-ffile-prefix-map={SCRIPT_DIR}=.",
           f"-fdebug-prefix-map={SCRIPT_DIR}=.", "-fdebug-compilation-dir=.",
           "-Wl,--pdb=greencurve.pdb"]
          if pdb_path else []),
        "-o",
        temp_output,
        *WINDOWS_SOURCE_FILES,
        ICON_RES,
        *WINDOWS_LINK_LIBS,
    ]


def get_windows_service_compile_command(temp_output, arch="x64", pdb_path=None):
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
            "-fno-lto",
            "-ftrivial-auto-var-init=pattern",
            "-fno-delete-null-pointer-checks",
            "-static",
            "-s",
            "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
            "-DGREEN_CURVE_SERVICE_BINARY=1",
            "-o",
            temp_output,
            *WINDOWS_SOURCE_FILES,
            ICON_RES,
            *WINDOWS_SERVICE_LINK_LIBS,
        ]
    return [
        LLVM_MINGW_CLANG,
        *COMMON_FLAGS,
        *WINDOWS_FLAGS,
        *(["-gcodeview", f"-ffile-prefix-map={SCRIPT_DIR}=.",
           f"-fdebug-prefix-map={SCRIPT_DIR}=.", "-fdebug-compilation-dir=.",
           "-Wl,--pdb=greencurve-service.pdb"]
          if pdb_path else []),
        "-DGREEN_CURVE_SERVICE_BINARY=1",
        "-o",
        temp_output,
        *WINDOWS_SOURCE_FILES,
        ICON_RES,
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


def _compile_only_flags(flags):
    """Return only front-end flags; never leak linker arguments into metadata/objects."""
    return [flag for flag in flags
            if not flag.startswith("-Wl,") and not flag.startswith("-l")
            and flag not in ("-static", "-s", "-pie", "-flto")]


def _run_compiler(cmd, cwd=SCRIPT_DIR, allow_cfg_collision=False):
    """Run a compiler and reject every duplicate-symbol diagnostic except the
    documented llvm-mingw CFG shim collision.  Capturing output makes the broad
    linker allowance auditable instead of silently accepting unrelated duplicates.
    """
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    combined = (result.stdout or "") + (result.stderr or "")
    if combined:
        print(combined, end="" if combined.endswith("\n") else "\n")
    duplicate_lines = [line for line in combined.splitlines()
                       if "duplicate symbol" in line.lower() or "multiple definition" in line.lower()]
    unexpected = []
    for line in duplicate_lines:
        if allow_cfg_collision and "__guard_check_icall_fptr" in line:
            continue
        unexpected.append(line)
    if unexpected:
        print("ERROR: unexpected duplicate-symbol diagnostic:")
        for line in unexpected:
            print(f"  {line}")
        return 1
    return result.returncode


def _compile_arm64_objects(sources, object_dir, target, extra_flags=None):
    """Compile ARM64 translation units independently so branch protection is
    materialized before link.  LTO is deliberately disabled for the 0.19 release.
    """
    os.makedirs(object_dir, exist_ok=True)
    objects = []
    compile_flags = _compile_only_flags([*COMMON_FLAGS, *(extra_flags or [])])
    for index, source in enumerate(sources):
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(object_dir, f"{index:02d}-{stem}.o")
        cmd = [ZIG_EXE, "c++", *compile_flags, "-target", target,
               "-mbranch-protection=standard", "-fno-lto", "-O2", "-c", source, "-o", obj]
        if _run_compiler(cmd, cwd=object_dir) != 0:
            raise RuntimeError(f"ARM64 object compilation failed: {source}")
        objects.append(obj)
    return objects


def _link_arm64_windows(temp_output, sources, link_libs, symbol_path, service=False):
    work = prepare_work_subdir("arm64-windows-service" if service else "arm64-windows-gui")
    try:
        definitions = ["-DGREEN_CURVE_SERVICE_BINARY=1"] if service else []
        objects = _compile_arm64_objects(
            sources, os.path.join(work, "obj"), "aarch64-windows-gnu",
            [*definitions, "-g", "-gdwarf-4", f"-ffile-prefix-map={SCRIPT_DIR}=.",
             f"-fdebug-prefix-map={SCRIPT_DIR}=.", "-fdebug-compilation-dir=.",
             "-ftrivial-auto-var-init=pattern",
             "-fno-delete-null-pointer-checks"])
        scratch_output = os.path.join(work, "greencurve-service.exe" if service else "greencurve.exe")
        scratch_symbols = scratch_output + ".debug"
        cmd = [ZIG_EXE, "c++", "-target", "aarch64-windows-gnu",
               "-mbranch-protection=standard", "-fno-lto", "-static",
               "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
               "-o", scratch_output, *objects, ICON_RES, *link_libs]
        if _run_compiler(cmd, cwd=work) != 0:
            raise RuntimeError("ARM64 Windows link failed")
        objcopy = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-objcopy.exe")
        strip = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-strip.exe")
        if subprocess.run([objcopy, "--only-keep-debug", scratch_output, scratch_symbols],
                          cwd=work).returncode != 0:
            raise RuntimeError("ARM64 Windows private-symbol extraction failed")
        if subprocess.run([strip, "--strip-all", scratch_output], cwd=work).returncode != 0:
            raise RuntimeError("ARM64 Windows release strip failed")
        os.replace(scratch_output, temp_output)
        os.makedirs(os.path.dirname(symbol_path), exist_ok=True)
        os.replace(scratch_symbols, symbol_path)
    finally:
        cleanup_work_subdir(work)


def _link_arm64_linux(temp_output, sources):
    work = prepare_work_subdir("arm64-linux")
    try:
        objects = _compile_arm64_objects(
            sources, os.path.join(work, "obj"), LINUX_ARM64_TRIPLE,
            ["-fPIE", "-fstack-protector-strong", "-fexceptions", "-frtti"])
        cmd = [ZIG_EXE, "c++", "-target", LINUX_ARM64_TRIPLE,
               "-mbranch-protection=standard", "-fno-lto", "-O2", "-pie",
               "-Wl,-z,relro,-z,now", "-Wl,-z,noexecstack", "-s",
               "-o", temp_output, *objects, "-ldl", "-lpthread"]
        if _run_compiler(cmd, cwd=work) != 0:
            raise RuntimeError("ARM64 Linux link failed")
    finally:
        cleanup_work_subdir(work)


def _verify_pe_hardening(data, arch):
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE signature")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise RuntimeError("release PE is not PE32+")
    dll_chars = struct.unpack_from("<H", data, optional + 70)[0]
    required = 0x20 | 0x40 | 0x100  # high-entropy VA, ASLR, DEP
    if dll_chars & required != required:
        raise RuntimeError(f"PE hardening bits missing (DllCharacteristics=0x{dll_chars:04x})")
    if arch == "x64" and not dll_chars & 0x4000:
        raise RuntimeError("Windows x64 CFG metadata is missing")
    number_of_sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    section_table = optional + optional_size

    def rva_to_offset(rva):
        for index in range(number_of_sections):
            section = section_table + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
                "<IIII", data, section + 8)
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_pointer + (rva - virtual_address)
        return None

    for index in range(number_of_sections):
        section = section_table + index * 40
        characteristics = struct.unpack_from("<I", data, section + 36)[0]
        if characteristics & 0xA0000000 == 0xA0000000:
            raise RuntimeError("PE has a writable/executable section")
    import_rva, import_size = struct.unpack_from("<II", data, optional + 112 + 8)
    if not import_rva or not import_size:
        raise RuntimeError("PE import dependency table is missing")
    if arch == "x64":
        load_rva, load_size = struct.unpack_from("<II", data, optional + 112 + 10 * 8)
        load_off = rva_to_offset(load_rva) if load_rva else None
        if load_off is None or load_size < 144 or load_off + 144 > len(data):
            raise RuntimeError("Windows x64 load-config/CFG table is missing")
        guard_table = struct.unpack_from("<Q", data, load_off + 128)[0]
        guard_count = struct.unpack_from("<Q", data, load_off + 136)[0]
        if not guard_table or not guard_count:
            raise RuntimeError("Windows x64 CFG function table is empty")
    major, minor, patch, build = _parse_version_parts()
    resource_version = f"{major}.{minor}.{patch}.{build}".encode("utf-16le")
    if resource_version not in data:
        raise RuntimeError("PE VERSIONINFO does not match VERSION/BUILD_NUMBER")


def sanitize_pe_codeview_path(binary_path, pdb_basename):
    """Replace LLD's absolute RSDS PDB path with a non-private basename."""
    with open(binary_path, "r+b") as handle:
        data = bytearray(handle.read())
        if len(data) < 0x100 or data[:2] != b"MZ":
            raise RuntimeError("cannot sanitize CodeView path in a non-PE artifact")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        optional = pe + 24
        debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + 6 * 8)
        section_count = struct.unpack_from("<H", data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        sections = optional + optional_size

        def rva_to_offset(rva):
            for index in range(section_count):
                section = sections + index * 40
                virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
                    "<IIII", data, section + 8)
                if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                    return raw_pointer + (rva - virtual_address)
            return None

        debug_offset = rva_to_offset(debug_rva) if debug_rva else None
        replacement = pdb_basename.encode("utf-8") + b"\0"
        sanitized = 0
        if debug_offset is not None:
            for entry in range(debug_offset, debug_offset + debug_size, 28):
                if entry + 28 > len(data):
                    break
                debug_type = struct.unpack_from("<I", data, entry + 12)[0]
                data_size = struct.unpack_from("<I", data, entry + 16)[0]
                data_pointer = struct.unpack_from("<I", data, entry + 24)[0]
                if debug_type != 2 or data_pointer + data_size > len(data):
                    continue
                if data[data_pointer:data_pointer + 4] != b"RSDS" or data_size <= 24:
                    continue
                old_capacity = data_size - 24
                if len(replacement) > old_capacity:
                    raise RuntimeError("sanitized PDB basename exceeds CodeView path capacity")
                start = data_pointer + 24
                data[start:start + old_capacity] = replacement + b"\0" * (old_capacity - len(replacement))
                sanitized += 1
        if sanitized != 1:
            raise RuntimeError(f"expected one RSDS CodeView record, found {sanitized}")
        handle.seek(0)
        handle.write(data)
        handle.truncate()


def _verify_elf_hardening(data):
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError("not a little-endian ELF64 image")
    if struct.unpack_from("<H", data, 16)[0] != 3:
        raise RuntimeError("ELF is not PIE (ET_DYN)")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize = struct.unpack_from("<H", data, 54)[0]
    phnum = struct.unpack_from("<H", data, 56)[0]
    have_relro = False
    have_nx_stack = False
    have_bind_now = False
    needed_dependencies = 0
    for index in range(phnum):
        off = phoff + index * phentsize
        if off + 56 > len(data):
            raise RuntimeError("truncated ELF program headers")
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset = struct.unpack_from("<Q", data, off + 8)[0]
        p_filesz = struct.unpack_from("<Q", data, off + 32)[0]
        if p_type == 1 and p_flags & 3 == 3:
            raise RuntimeError("ELF has a writable/executable LOAD segment")
        if p_type == 0x6474E552:
            have_relro = True
        if p_type == 0x6474E551:
            have_nx_stack = not (p_flags & 1)
        if p_type == 2:
            end = min(len(data), p_offset + p_filesz)
            for dyn in range(p_offset, end, 16):
                if dyn + 16 > end:
                    break
                tag, value = struct.unpack_from("<QQ", data, dyn)
                if tag == 0:
                    break
                if tag == 24 or (tag == 30 and value & 8) or (tag == 0x6FFFFFFB and value & 1):
                    have_bind_now = True
                if tag == 1:
                    needed_dependencies += 1
    if not have_relro or not have_bind_now or not have_nx_stack:
        raise RuntimeError(f"ELF hardening incomplete (RELRO={have_relro}, BIND_NOW={have_bind_now}, NX={have_nx_stack})")
    if needed_dependencies == 0:
        raise RuntimeError("ELF dynamic dependency table is missing")


def verify_release_binary(path, os_name, arch, allow_debug_paths=False):
    """Mandatory post-link artifact verification, independent of command flags."""
    with open(path, "rb") as handle:
        data = handle.read()
    actual = detect_binary_arch(path)
    if actual != arch:
        raise RuntimeError(f"architecture is {actual or 'unknown'}, expected {arch}")
    if APP_VERSION.encode("ascii") not in data:
        raise RuntimeError(f"version metadata does not contain {APP_VERSION}")
    workspace_markers = {
        os.path.abspath(SCRIPT_DIR).encode("utf-8", "ignore"),
        os.path.abspath(SCRIPT_DIR).replace("\\", "/").encode("utf-8", "ignore"),
    }
    if not allow_debug_paths and any(marker and marker in data for marker in workspace_markers):
        raise RuntimeError("binary embeds the private build workspace path")
    if os_name == "windows":
        _verify_pe_hardening(data, arch)
    else:
        _verify_elf_hardening(data)
    if arch == "arm64":
        bti = data.count(b"\x5f\x24\x03\xd5")
        # AAPCS/Linux generally uses key A; Windows ARM64 uses key B.
        pac = data.count(b"\x3f\x23\x03\xd5") + data.count(b"\x7f\x23\x03\xd5")
        aut = data.count(b"\xbf\x23\x03\xd5") + data.count(b"\xff\x23\x03\xd5")
        if bti == 0 or pac == 0 or aut == 0:
            raise RuntimeError(f"ARM64 branch protection missing (BTI={bti}, PAC={pac}, AUT={aut})")
        print(f"  ARM64 branch protection: BTI={bti}, PAC={pac}, AUT={aut}")
    print(f"  Verified {os_name}-{arch}: architecture, version, hardening, sections, private paths")


def verify_windows_private_symbols(pdb_path, arch):
    """Require readable private postmortem symbols for every Windows artifact."""
    if not os.path.isfile(pdb_path) or os.path.getsize(pdb_path) < 4096:
        raise RuntimeError(f"matching Windows {arch} symbols are missing or empty: {pdb_path}")
    if arch == "arm64":
        readobj = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-readobj.exe")
        result = subprocess.run([readobj, "--sections", pdb_path],
                                text=True, capture_output=True)
        if result.returncode != 0 or ".debug_info" not in result.stdout or \
                ".debug_line" not in result.stdout:
            raise RuntimeError(f"private ARM64 debug file failed structural verification: {pdb_path}")
        print(f"  Verified private symbols: {pdb_path} ({os.path.getsize(pdb_path):,} bytes)")
        return
    pdbutil = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-pdbutil.exe")
    if not os.path.isfile(pdbutil):
        raise RuntimeError("llvm-pdbutil is missing; cannot verify private symbols")
    result = subprocess.run([pdbutil, "dump", "-summary", pdb_path],
                            text=True, capture_output=True)
    if result.returncode != 0 or "GUID:" not in result.stdout or \
            "Has Debug Info: true" not in result.stdout:
        raise RuntimeError(f"private PDB failed structural verification: {pdb_path}")
    print(f"  Verified private symbols: {pdb_path} ({os.path.getsize(pdb_path):,} bytes)")


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

    pdb_path = windows_symbol_output_path(output_path, arch)
    link_pdb_path = os.path.join(SCRIPT_DIR, "greencurve.pdb")
    os.makedirs(os.path.dirname(pdb_path), exist_ok=True)
    if os.path.exists(pdb_path):
        os.remove(pdb_path)
    if arch == "x64" and os.path.exists(link_pdb_path):
        os.remove(link_pdb_path)
    cmd = get_windows_gui_compile_command(temp_output, arch, pdb_path)

    print(f"Compiling {len(WINDOWS_SOURCE_FILES)} source files -> {os.path.basename(output_path)} ({arch})")
    print("  Mode: object-first Zig ARM64, LTO disabled" if arch == "arm64"
          else f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_windows(temp_output, WINDOWS_SOURCE_FILES, WINDOWS_LINK_LIBS,
                                pdb_path)
            returncode = 0
        else:
            returncode = _run_compiler(cmd, allow_cfg_collision=True)
            if returncode == 0:
                os.replace(link_pdb_path, pdb_path)
        if returncode == 0:
            if arch == "x64":
                sanitize_pe_codeview_path(temp_output, os.path.basename(pdb_path))
            verify_release_binary(temp_output, "windows", arch, "-g" in COMMON_FLAGS)
            verify_windows_private_symbols(pdb_path, arch)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}")
        returncode = 1
    if returncode != 0:
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

    pdb_path = windows_symbol_output_path(output_path, arch)
    link_pdb_path = os.path.join(SCRIPT_DIR, "greencurve-service.pdb")
    os.makedirs(os.path.dirname(pdb_path), exist_ok=True)
    if os.path.exists(pdb_path):
        os.remove(pdb_path)
    if arch == "x64" and os.path.exists(link_pdb_path):
        os.remove(link_pdb_path)
    cmd = get_windows_service_compile_command(temp_output, arch, pdb_path)

    print(f"Compiling {len(WINDOWS_SOURCE_FILES)} source files -> {os.path.basename(output_path)} ({arch})")
    print("  Mode: object-first Zig ARM64, LTO disabled" if arch == "arm64"
          else f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_windows(temp_output, WINDOWS_SOURCE_FILES,
                                WINDOWS_SERVICE_LINK_LIBS, pdb_path, service=True)
            returncode = 0
        else:
            returncode = _run_compiler(cmd, allow_cfg_collision=True)
            if returncode == 0:
                os.replace(link_pdb_path, pdb_path)
        if returncode == 0:
            if arch == "x64":
                sanitize_pe_codeview_path(temp_output, os.path.basename(pdb_path))
            verify_release_binary(temp_output, "windows", arch, "-g" in COMMON_FLAGS)
            verify_windows_private_symbols(pdb_path, arch)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}")
        returncode = 1
    if returncode != 0:
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
    print("  Mode: object-first Zig ARM64, LTO disabled" if arch == "arm64"
          else f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_linux(temp_output, LINUX_SOURCE_FILES)
            returncode = 0
        else:
            returncode = _run_compiler(cmd)
        if returncode == 0:
            verify_release_binary(temp_output, "linux", arch, "-g" in COMMON_FLAGS)
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}")
        returncode = 1
    if returncode != 0:
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


def expected_release_names(os_name):
    if os_name == "windows":
        return {"greencurve.exe", "greencurve-service.exe", "README.md", "LICENSE"}
    if os_name == "linux":
        return {"greencurve", "README.md", "LICENSE"}
    raise ValueError(f"unsupported release OS: {os_name}")


def validate_payload_file_names(payload, expected_binary_names):
    """Reject stale/linker side products before any archive is created."""
    actual = {name for name in os.listdir(payload)
              if os.path.isfile(os.path.join(payload, name))}
    unexpected = actual - set(expected_binary_names)
    missing = set(expected_binary_names) - actual
    if unexpected or missing:
        raise RuntimeError(f"payload manifest mismatch: missing={sorted(missing)}, unexpected={sorted(unexpected)}")


def _verify_archive_manifest(seven, archive, expected_names):
    result = subprocess.run([seven, "l", "-slt", archive], text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError("7-Zip could not inspect the completed archive")
    in_entries = False
    paths = set()
    for line in result.stdout.splitlines():
        if line.startswith("----------"):
            in_entries = True
            continue
        if in_entries and line.startswith("Path = "):
            paths.add(line[7:].replace("\\", "/"))
    expected_paths = {"greencurve", *(f"greencurve/{name}" for name in expected_names)}
    if paths != expected_paths:
        raise RuntimeError(f"archive manifest mismatch: expected={sorted(expected_paths)}, actual={sorted(paths)}")


def package_release_archive(os_name, arch, binaries):
    """Stage and archive an exact per-platform allowlist, then read it back."""
    payload = target_payload_dir(os_name, arch)
    binary_names = {os.path.basename(path) for path in binaries}
    expected_names = expected_release_names(os_name)
    if binary_names != expected_names - {"README.md", "LICENSE"}:
        raise RuntimeError(f"binary allowlist mismatch for {os_name}: {sorted(binary_names)}")
    for binary in binaries:
        if not os.path.exists(binary):
            raise RuntimeError(f"missing build output {binary}")
        actual_arch = detect_binary_arch(binary)
        if actual_arch != arch:
            raise RuntimeError(f"architecture mismatch: {binary} is {actual_arch}, expected {arch}")
    validate_payload_file_names(payload, binary_names)
    for required in (README_MD_PATH, LICENSE_PATH):
        if not os.path.isfile(required):
            raise RuntimeError(f"required release file missing: {required}")

    seven = find_seven_zip()
    if not seven:
        raise RuntimeError("7-Zip is required to produce verified release archives")
    archive = os.path.join(SCRIPT_DIR, f"greencurve-{APP_VERSION}-{os_name}-{arch}.7z")
    if os.path.exists(archive):
        os.remove(archive)
    work = prepare_work_subdir(f"package-{os_name}-{arch}")
    try:
        staging = os.path.join(work, "greencurve")
        os.makedirs(staging, exist_ok=True)
        for binary in binaries:
            shutil.copy2(binary, os.path.join(staging, os.path.basename(binary)))
        for extra in (README_MD_PATH, LICENSE_PATH):
            shutil.copy2(extra, os.path.join(staging, os.path.basename(extra)))
        validate_payload_file_names(staging, expected_names)
        result = subprocess.run(
            [seven, "a", "-t7z", "-mx=9", "-bso0", "-bsp0", archive, "greencurve"],
            cwd=work,
        )
        if result.returncode != 0 or not os.path.exists(archive):
            raise RuntimeError(f"7-Zip archiving failed for {os_name}-{arch}")
        _verify_archive_manifest(seven, archive, expected_names)
    finally:
        cleanup_work_subdir(work)
    size = os.path.getsize(archive)
    print(f"Archived {os.path.basename(archive)} ({size:,} bytes / {size / 1024:.1f} KB)")
    hash_path = archive + ".sha256"
    with open(hash_path, "w") as f:
        f.write(f"{_sha256_file(archive)}  {os.path.basename(archive)}\n")


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


def requested_arches(arch):
    if arch == "all":
        return ["x64", "arm64"]
    if arch in ("x64", "arm64"):
        return [arch]
    raise ValueError(f"unsupported architecture: {arch}")


def run_check_builds(target, arch="all", generate_lsp=True):
    """Build selected targets into a temporary directory without replacing release outputs."""
    if generate_lsp:
        generate_lsp_files()
    tmp = prepare_work_subdir("check")
    try:
        for selected_arch in requested_arches(arch):
            arch_tmp = os.path.join(tmp, selected_arch)
            os.makedirs(arch_tmp, exist_ok=True)
            if target in ("windows", "all"):
                compile_windows_binary(
                    output_path=os.path.join(arch_tmp, "greencurve.exe"),
                    temp_output=os.path.join(arch_tmp, "greencurve.exe.new"),
                    backup_path="", arch=selected_arch, finalize=False,
                )
                compile_windows_service_binary(
                    output_path=os.path.join(arch_tmp, "greencurve-service.exe"),
                    temp_output=os.path.join(arch_tmp, "greencurve-service.exe.new"),
                    backup_path="", arch=selected_arch, finalize=False,
                )
            if target in ("linux", "all"):
                suffix = LINUX_ARM64_TRIPLE if selected_arch == "arm64" else LINUX_TARGET
                compile_linux_binary(
                    output_path=os.path.join(arch_tmp, f"greencurve-{suffix}"),
                    temp_output=os.path.join(arch_tmp, f"greencurve-{suffix}.new"),
                    backup_path="", arch=selected_arch, finalize=False,
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
        if flag.startswith("-Wl,") or flag.startswith("-l"):
            continue
        result.append(flag)
    return result


def zig_linux_analyzer_flags(arch="x64"):
    """Expose Zig's target headers explicitly to clangd/clang-tidy.  A raw
    clang++ compile database otherwise knows the target triple but not Zig's
    glibc/libc++ sysroot and reports every standard header as missing.
    """
    triple_dir = "x86_64-linux-gnu" if arch == "x64" else "aarch64-linux-gnu"
    include_dirs = [
        os.path.join(ZIG_DIR, "lib", "libcxx", "include"),
        os.path.join(ZIG_DIR, "lib", "libcxxabi", "include"),
        os.path.join(ZIG_DIR, "lib", "libunwind", "include"),
        os.path.join(ZIG_DIR, "lib", "include"),
        os.path.join(ZIG_DIR, "lib", "libc", "include", triple_dir),
        os.path.join(ZIG_DIR, "lib", "libc", "include", "generic-glibc"),
        os.path.join(ZIG_DIR, "lib", "libc", "include", "x86-linux-any"),
        os.path.join(ZIG_DIR, "lib", "libc", "include", "any-linux-any"),
    ]
    flags = [
        "-nostdinc", "-nostdinc++", "-D__GLIBC_MINOR__=28",
        "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
        "-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
        "-D_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS",
        "-D_LIBCPP_PSTL_CPU_BACKEND_SERIAL",
        "-D_LIBCPP_ABI_VERSION=1", "-D_LIBCPP_ABI_NAMESPACE=__1",
        "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG",
    ]
    for directory in include_dirs:
        flags.extend(["-isystem", directory])
    return flags


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
            "arguments": ["clang++", *linux_flags, *zig_linux_analyzer_flags(),
                          "-fsyntax-only", source],
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
        if requested_arches("all") != ["x64", "arm64"]:
            print("Build-script regression FAILED: --arch all does not select both architectures")
            sys.exit(1)
        payload = os.path.join(tmp, "payload")
        os.makedirs(payload)
        expected = {"greencurve.exe", "greencurve-service.exe"}
        for name in expected:
            with open(os.path.join(payload, name), "wb") as handle:
                handle.write(b"fixture")
        validate_payload_file_names(payload, expected)
        with open(os.path.join(payload, "main.lib"), "wb") as handle:
            handle.write(b"unexpected linker side product")
        try:
            validate_payload_file_names(payload, expected)
        except RuntimeError:
            pass
        else:
            print("Build-script regression FAILED: unexpected package file accepted")
            sys.exit(1)
    finally:
        cleanup_work_subdir(tmp)


def run_regression_tests(extra_flags=None):
    """Run pure regression tests that do not touch GPU hardware."""
    run_build_script_regression_tests()
    harness = r'''
#include "fan_curve.h"
#include <string>
#include "lock_checkbox_policy.h"
#include "main_layout_policy.h"
#include "ui_theme_metrics.h"
#include "ui_checkbox_state.h"
#include "service_acl.h"
#include "vf_backends.h"
#include "service_lifecycle_policy.h"
#include "service_recovery_policy.h"
#include "selected_gpu_pnp_policy.h"
#include "gpu_selection_policy.h"
#include "linux_gpu_selection.h"
#include "linux_daemon_state.h"
#include "linux_transaction.h"
#include "profile_persistence_policy.h"
#include "profile_startup_policy.h"
#include "startup_task_definition_policy.h"
#include "linux_tui_layout.cpp"

// The task classifier lives in an amalgamated Windows shard.  Supply only the
// surrounding declarations needed to compile that shard into this fixture;
// executable tests call the pure XML classifier and never query Task Scheduler.
static char g_userDataDir[MAX_PATH] = {};
static WCHAR g_forcedStartupUserSam[512] = {};
bool utf8_to_wide(const char*, WCHAR*, int);
static bool get_current_user_sam_name(WCHAR*, DWORD) { return false; }
#include "main_startup_task_definition.cpp"

bool is_curve_point_visible_in_gui(int) { return true; }
void debug_log(const char*, ...) {}
#include "config_profile_repair.cpp"

void invalidate_tray_profile_cache() {}

// Production auto-profile persistence uses the amalgamated atomic whole-file
// section writer.  The pure fixture starts with an empty temporary INI, so this
// deterministic stand-in only needs to commit the supplied complete sections.
static bool write_config_sections_atomic(const char* path,
    const char* newSectionsData, const char* const*, int,
    char* err, size_t errSize) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "fixture CreateFile failed");
        return false;
    }
    DWORD size = (DWORD)strlen(newSectionsData);
    DWORD written = 0;
    bool ok = WriteFile(file, newSectionsData, size, &written, nullptr) &&
        written == size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

// Auto-profile pure core (resolver + controller state machine).  Included
// directly so the coalescing/hysteresis/manual-pin logic and the rule resolver
// are exercised without a GPU or an interactive desktop.
#include "auto_profile_rules.cpp"
#include "auto_profile_controller.cpp"
#include "hotkeys.cpp"

#if defined(_WIN32)
static unsigned int g_nativeLockClickedCount = 0;
static unsigned int g_nativeLockDoubleClickedCount = 0;

static LRESULT CALLBACK native_lock_test_parent_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == 7001) {
        if (HIWORD(wParam) == BN_CLICKED) g_nativeLockClickedCount++;
        if (HIWORD(wParam) == BN_DBLCLK) g_nativeLockDoubleClickedCount++;
        return 0;
    }
    if (msg == WM_DRAWITEM) return TRUE;
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool run_native_ownerdraw_button_notification_test() {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    const char* className = "GreenCurveLockPolicyRegressionWindow";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = native_lock_test_parent_proc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassA(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    HWND parent = CreateWindowExA(0, className, "", WS_OVERLAPPED,
                                  0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    if (!parent) return false;
    HWND button = CreateWindowExA(0, "BUTTON", "",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 30, 20, parent, (HMENU)(INT_PTR)7001, instance, nullptr);
    if (!button) {
        DestroyWindow(parent);
        return false;
    }
    g_nativeLockClickedCount = 0;
    g_nativeLockDoubleClickedCount = 0;
    SendMessageA(button, BM_CLICK, 0, 0);
    SendMessageA(button, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(5, 5));
    bool ok = g_nativeLockClickedCount == 1 && g_nativeLockDoubleClickedCount == 1;
    DestroyWindow(parent);
    UnregisterClassA(className, instance);
    return ok;
}
#endif

struct FakeLinuxTransaction {
    unsigned int failPhase;
    unsigned int calls[7];
    unsigned int callCount;
    unsigned int rollbackMask;
    bool rollbackOk;
};

static bool fake_linux_transaction_step(void* opaque, unsigned int phase) {
    FakeLinuxTransaction* fake = (FakeLinuxTransaction*)opaque;
    if (fake->callCount < 7) fake->calls[fake->callCount++] = phase;
    return phase != fake->failPhase;
}

static bool fake_linux_transaction_rollback(void* opaque, unsigned int attempted) {
    FakeLinuxTransaction* fake = (FakeLinuxTransaction*)opaque;
    fake->rollbackMask = attempted;
    return fake->rollbackOk;
}

int main(int argc, char** argv) {
    InitializeCriticalSection(&g_configLock);

    if (APP_DEBUG_DEFAULT_ENABLED != 1) return 20;

    // Merely selecting a saved slot must not make saved OC intent look live on
    // the next GUI launch. Only explicit app-launch automation may source the
    // startup editor from a profile; disabled/invalid assignments show hardware.
    if (startup_editor_source(false, 0, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 722;
    if (startup_editor_source(false, -1, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 723;
    if (startup_editor_source(false, 6, 5) != STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT) return 724;
    if (startup_editor_source(false, 2, 5) != STARTUP_EDITOR_SOURCE_APP_LAUNCH_PROFILE) return 725;
    if (startup_editor_source(true, 2, 5) != STARTUP_EDITOR_SOURCE_LOGON_SERVICE) return 726;

    // Responsive main-window layout: the reported 3440x1440/custom-140% case
    // must fit by shrinking only the graph, while pathological effective work
    // areas retain the full content canvas and advertise scroll overflow.
    {
        const int dpi = 134; // Windows custom 140% is approximately 134 DPI.
        const int width = main_layout_scale_px(MAIN_LAYOUT_BASE_WIDTH_LOGICAL, dpi);
        MainLayoutPlan reported = main_layout_build_plan(width, 1330, dpi, 87);
        if (reported.columns != 6 || reported.rowsPerColumn != 15) return 700;
        if (reported.horizontalOverflow || reported.verticalOverflow) return 701;
        if (reported.contentHeight != reported.viewportHeight) return 702;
        if (reported.graphHeight < main_layout_scale_px(MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, dpi) ||
            reported.graphHeight >= main_layout_scale_px(MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL, dpi)) return 703;

        MainLayoutPlan expanded = main_layout_build_plan(3000, 1330, dpi, 87);
        if (expanded.columns != 11 || expanded.rowsPerColumn != 8) return 704;
        if (expanded.verticalOverflow || expanded.horizontalOverflow) return 705;

        MainLayoutPlan constrained = main_layout_build_plan(1200, 650, 288, 87);
        if (!constrained.horizontalOverflow || !constrained.verticalOverflow) return 706;
        if (constrained.graphHeight !=
            main_layout_scale_px(MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, 288)) return 707;

        MainLayoutPlan empty = main_layout_build_plan(1920, 1000, 144, 0);
        if (empty.columns != 0 || empty.rowsPerColumn != 0) return 708;

        const int dpis[] = { 96, 110, 120, 134, 144, 168, 192, 216, 240, 288 };
        const int widths[] = { 720, 960, 1280, 1600, 1920, 2560, 3440, 3840 };
        const int heights[] = { 480, 650, 720, 900, 1000, 1040, 1330, 1376, 1400, 2120 };
        for (int testDpi : dpis) {
            for (int testWidth : widths) {
                for (int testHeight : heights) {
                    MainLayoutPlan plan = main_layout_build_plan(
                        testWidth, testHeight, testDpi, 87);
                    if (plan.columns < 1 || plan.columns > MAIN_LAYOUT_MAX_COLUMNS ||
                        plan.rowsPerColumn != (87 + plan.columns - 1) / plan.columns) return 709;
                    if (plan.graphHeight < main_layout_scale_px(
                            MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, testDpi) ||
                        plan.graphHeight > main_layout_scale_px(
                            MAIN_LAYOUT_GRAPH_MAX_HEIGHT_LOGICAL, testDpi)) return 710;
                    if (!(plan.graphHeight < plan.pointStartY &&
                          plan.pointStartY <= plan.globalControlsY &&
                          plan.globalControlsY < plan.buttonsY &&
                          plan.buttonsY < plan.profileY &&
                          plan.profileY < plan.autoY && plan.autoY < plan.sharedY &&
                          plan.sharedY < plan.serviceY && plan.serviceY < plan.hintY &&
                          plan.hintY < plan.statusY && plan.statusY < plan.contentHeight)) return 711;
                    if (plan.horizontalOverflow != (plan.contentWidth > testWidth) ||
                        plan.verticalOverflow != (plan.contentHeight > testHeight)) return 712;
                    MainLayoutPointCell previous = {};
                    for (int vi = 0; vi < 87; ++vi) {
                        MainLayoutPointCell cell = main_layout_point_cell(plan, vi);
                        if (cell.left < 0 || cell.top < plan.pointStartY ||
                            cell.right > plan.contentWidth ||
                            cell.bottom > plan.globalControlsY -
                                main_layout_scale_px(6, testDpi)) return 713;
                        if (vi > 0 && cell.left == previous.left &&
                            cell.top != previous.bottom) return 714;
                        if (vi > 0 && cell.left != previous.left &&
                            (cell.left <= previous.left || cell.top != plan.pointStartY)) return 715;
                        previous = cell;
                    }
                    if (main_layout_scale_px(1006 + 160 + 8, testDpi) >
                        plan.contentWidth) return 716;
                    if (plan.graphHeight - main_layout_scale_px(35 + 55, testDpi) <
                        main_layout_scale_px(160, testDpi)) return 717;
                }
            }
        }
        MainLayoutSize grown = main_layout_grow_size(
            1792, 1123, 1792, 1513, 3840, 2080);
        if (grown.width != 1792 || grown.height != 1513) return 718;
        MainLayoutSize workClamped = main_layout_grow_size(
            1792, 1123, 1792, 1513, 3440, 1376);
        if (workClamped.width != 1792 || workClamped.height != 1376) return 719;
        MainLayoutSize noShrink = main_layout_grow_size(
            2200, 1700, 1792, 1513, 3840, 2080);
        if (noShrink.width != 2200 || noShrink.height != 1700) return 720;
        // Captured 4K/150% regression: the service populated 78 VF points after
        // initial empty-state sizing. Growing to the populated preferred height
        // must restore the full graph and six-column grid without overflow.
        int populatedHeight = main_layout_preferred_client_height(1770, 144, 78);
        MainLayoutPlan populated = main_layout_build_plan(1770, populatedHeight, 144, 78);
        if (populated.columns != 6 || populated.rowsPerColumn != 13
            || populated.graphHeight != main_layout_scale_px(420, 144)
            || populated.horizontalOverflow || populated.verticalOverflow) return 721;

        MainLayoutRect work = { 100, 50, 2100, 1250 };
        MainLayoutRect centered = main_layout_center_rect(work, 1000, 600);
        if (centered.left != 600 || centered.top != 350 ||
            centered.right != 1600 || centered.bottom != 950) return 727;
        MainLayoutRect oversized = main_layout_center_rect(work, 4000, 3000);
        if (oversized.left != work.left || oversized.top != work.top ||
            oversized.right != work.right || oversized.bottom != work.bottom) return 728;
        MainLayoutRect current = { 300, 200, 1300, 800 };
        MainLayoutRect resized = main_layout_resize_around_center(
            current, 1200, 800);
        if (resized.left != 200 || resized.top != 100 ||
            resized.right != 1400 || resized.bottom != 900) return 729;

        const int checkboxDpi = 144;
        int unlabeledBox = ui_theme_checkbox_box_size(
            ui_theme_scale_px(16, checkboxDpi),
            ui_theme_scale_px(16, checkboxDpi), checkboxDpi);
        int labeledBox = ui_theme_checkbox_box_size(
            ui_theme_scale_px(240, checkboxDpi),
            ui_theme_scale_px(22, checkboxDpi), checkboxDpi);
        if (unlabeledBox != ui_theme_scale_px(14, checkboxDpi) ||
            labeledBox != unlabeledBox) return 730;

        UiCheckboxState checkboxState = {};
        if (ui_checkbox_state_get(&checkboxState)) return 735;
        ui_checkbox_state_set(&checkboxState, true);
        if (!ui_checkbox_state_get(&checkboxState)) return 736;
        if (ui_checkbox_state_toggle(&checkboxState) ||
            ui_checkbox_state_get(&checkboxState)) return 737;
    }

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
    HANDLE configPeer = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE,
        FALSE, "Global\\GreenCurveConfigMutex-v2");
    if (!configPeer) return 522;
    CloseHandle(configPeer);
    HANDLE overprivilegedConfigPeer = OpenMutexA(MUTEX_ALL_ACCESS,
        FALSE, "Global\\GreenCurveConfigMutex-v2");
    if (overprivilegedConfigPeer) {
        CloseHandle(overprivilegedConfigPeer);
        return 528;
    }
    if (enter_config_storage_lock(nullptr)) return 529;
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
    {
        ConfiguredGpuSelection published = {};
        published.stableIdentityPresent = true;
        published.legacyIndex = 2;
        published.identity.valid = true;
        published.identity.pciInfoValid = true;
        published.identity.deviceId = 0x268410DEu;
        published.identity.subSystemId = 0x17AA3A5Cu;
        published.identity.pciRevisionId = 0xA1u;
        published.identity.extDeviceId = 0x12345678u;
        published.identity.pciDomain = 0;
        published.identity.pciBus = 9;
        published.identity.pciDevice = 3;
        published.identity.pciFunction = 1;
        char section[1024] = {};
        char gpuErr[256] = {};
        if (!format_configured_gpu_selection_section("profile2_gpu",
                &published, section, sizeof(section), gpuErr,
                sizeof(gpuErr))) return 731;
        const char* replaced[] = { "profile2_gpu" };
        if (!write_config_sections_atomic(argv[1], section, replaced, 1,
                gpuErr, sizeof(gpuErr))) return 732;
        ConfiguredGpuSelection loaded = {};
        if (!load_configured_gpu_selection_from_section(argv[1],
                "profile2_gpu", &loaded, gpuErr, sizeof(gpuErr))) return 733;
        if (!loaded.stableIdentityPresent || loaded.legacyIndex != 2 ||
            !configured_gpu_base_identity_matches(
                &published.identity, &loaded.identity) ||
            loaded.identity.pciBus != 9 || loaded.identity.pciDevice != 3 ||
            loaded.identity.pciFunction != 1) return 734;
    }
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

    // Lock-checkbox activation policy: one accepted transition per armed
    // mouse/key gesture, non-click notifications are inert, and a startup or
    // service synchronization between press and release fails safe instead of
    // turning the newly arrived FLATTEN state into HARD.
    {
        LockUiStateStamp none = {-1, -1, 0u, LOCK_MODE_NONE};
        LockUiStateStamp flatten = {27, 76, 2957u, LOCK_MODE_FLATTEN};
        if (lock_mode_after_activation(false, LOCK_MODE_NONE) != LOCK_MODE_FLATTEN) return 624;
        if (lock_mode_after_activation(true, LOCK_MODE_FLATTEN) != LOCK_MODE_HARD) return 625;
        if (lock_mode_after_activation(true, LOCK_MODE_HARD) != LOCK_MODE_NONE) return 626;
        if (decide_lock_activation(BN_SETFOCUS, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_IGNORE_NOTIFICATION) return 627;
        if (decide_lock_activation(BN_DBLCLK, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_IGNORE_NOTIFICATION) return 628;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, false, false, -1, 27, none, none)
                != LOCK_ACTIVATION_ACCEPT_UNARMED) return 629;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 27, 27, none, none)
                != LOCK_ACTIVATION_ACCEPT_ARMED) return 630;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, true, 27, 27, none, none)
                != LOCK_ACTIVATION_REJECT_ALREADY_CONSUMED) return 631;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 26, 27, none, none)
                != LOCK_ACTIVATION_REJECT_WRONG_CONTROL) return 632;
        if (decide_lock_activation(BN_CLICKED, BN_CLICKED, true, false, 27, 27, none, flatten)
                != LOCK_ACTIVATION_REJECT_STATE_CHANGED) return 633;
        if (!lock_ui_state_stamp_equal(flatten, flatten) || lock_ui_state_stamp_equal(none, flatten)) return 634;
#if defined(_WIN32)
        if (!run_native_ownerdraw_button_notification_test()) return 635;
#endif
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
        if (resolve_logon_profile_source(true, false, 3, true, 1, true, true) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 140;
        if (resolve_logon_profile_source(false, true, 2, true, 0, false, false) != LOGON_PROFILE_SOURCE_SHARED_BANK) return 141;
        // Restricted + only a per-user slot => bypass closed (none, no default).
        if (resolve_logon_profile_source(true, false, 0, false, 1, true, false) != LOGON_PROFILE_SOURCE_NONE) return 142;
        // Restricted + per-user slot + machine default => the shared default.
        if (resolve_logon_profile_source(true, false, 0, false, 1, true, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 143;
        // An explicit shared choice never silently degrades to another profile.
        if (resolve_logon_profile_source(true, false, 4, false, 0, false, true) != LOGON_PROFILE_SOURCE_PENDING) return 144;
        // Admin under policy keeps the per-user slot.
        if (resolve_logon_profile_source(true, true, 0, false, 2, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 145;
        // Policy off, non-admin: per-user slot honored.
        if (resolve_logon_profile_source(false, false, 0, false, 2, true, true) != LOGON_PROFILE_SOURCE_PER_USER) return 146;
        // Policy off, non-admin, no per-user but machine default => default.
        if (resolve_logon_profile_source(false, false, 0, false, 0, false, true) != LOGON_PROFILE_SOURCE_MACHINE_DEFAULT) return 147;
        // Nothing available => none (both unrestricted and restricted).
        if (resolve_logon_profile_source(false, false, 0, false, 0, false, false) != LOGON_PROFILE_SOURCE_NONE) return 148;
        if (resolve_logon_profile_source(true, false, 0, false, 0, false, false) != LOGON_PROFILE_SOURCE_NONE) return 149;
        // Unrestricted explicit personal slot that is missing remains pending.
        if (resolve_logon_profile_source(false, false, 0, false, 3, false, true) != LOGON_PROFILE_SOURCE_PENDING) return 601;
    }

    // Stable GPU selection survives enumeration reordering and fails closed
    // when an identical identity is ambiguous or absent.
    {
        ConfiguredGpuSelection configured = {};
        configured.stableIdentityPresent = true;
        configured.legacyIndex = 0;
        configured.identity.valid = true;
        configured.identity.pciInfoValid = true;
        configured.identity.deviceId = 0x1234;
        configured.identity.subSystemId = 0x5678;
        configured.identity.pciRevisionId = 1;
        configured.identity.extDeviceId = 2;
        configured.identity.pciBus = 9;
        configured.identity.pciDevice = 0;
        GpuAdapterInfo adapters[2] = {};
        adapters[0] = configured.identity;
        adapters[0].pciBus = 3;
        adapters[1] = configured.identity;
        unsigned int resolved = 99;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_STABLE || resolved != 1) return 602;
        adapters[1].pciBus = 3;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_NOT_FOUND) return 603;
        configured.identity.pciBus = 0;
        configured.identity.pciDevice = 0;
        adapters[0] = configured.identity;
        adapters[1] = configured.identity;
        if (resolve_configured_gpu_selection(&configured, adapters, 2, &resolved) != CONFIGURED_GPU_RESOLVE_AMBIGUOUS) return 604;
    }

    // explicit_vf_points_v1 makes lock_ci=-1 an unlocked custom curve, not a
    // legacy captured-live-curve cleanup marker.
    if (profile_should_strip_legacy_unlocked_curve(true, -1, true)) return 605;
    if (!profile_should_strip_legacy_unlocked_curve(true, -1, false)) return 606;

    // Apply origins are security policy, not diagnostic strings.  Only a
    // successful explicit user action may clear a sticky automatic-restore
    // lockout.  Every automatic origin, including app-launch/foreground, must
    // honor it.  Service startup has no apply origin at all.
    {
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_GUI)) return 160;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_CLI)) return 161;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_HOTKEY)) return 162;
        if (!service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_TRAY)) return 163;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 164;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 165;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_LOGON)) return 166;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_STANDBY)) return 167;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 168;
        if (service_apply_origin_is_explicit(SERVICE_APPLY_ORIGIN_UNSPECIFIED)) return 169;

        if (service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_GUI)) return 170;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 171;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 172;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_LOGON)) return 173;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_STANDBY)) return 174;
        if (!service_apply_origin_is_automatic(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 175;

        // Client APPLY may carry explicit actions and the two GUI-owned
        // automation origins only. Logon has a settings-free command, while
        // standby/driver recovery are authorized only by the lifecycle worker.
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_GUI)) return 555;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_CLI)) return 556;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_HOTKEY)) return 557;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_TRAY)) return 558;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_APP_LAUNCH)) return 559;
        if (!service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_FOREGROUND)) return 560;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_LOGON)) return 561;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_STANDBY)) return 562;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)) return 563;
        if (service_apply_origin_is_client_apply(SERVICE_APPLY_ORIGIN_UNSPECIFIED)) return 564;

        if (SERVICE_CMD_LOGON_HANDOFF == SERVICE_CMD_APPLY) return 176;
        if (SERVICE_CMD_LOGON_HANDOFF == SERVICE_CMD_RESET) return 177;

        ServiceSnapshot snapshot = {};
        snapshot.activeProfileSource = 999;
        snapshot.activeProfileSlot = 999;
        snapshot.lastLifecycleTrigger = 999;
        snapshot.lastLifecycleResult = 999;
        snapshot.autoRestoreLockoutReason = 999;
        validate_service_snapshot_for_ipc(&snapshot);
        if (snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE) return 178;
        if (snapshot.activeProfileSlot != 0) return 179;
        if (snapshot.lastLifecycleTrigger != SERVICE_LIFECYCLE_TRIGGER_NONE) return 180;
        if (snapshot.lastLifecycleResult != SERVICE_LIFECYCLE_RESULT_NONE) return 181;
        // Invalid lockout metadata fails closed rather than silently rearming.
        if (snapshot.autoRestoreLockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) return 182;
    }

    // Exact selected-GPU PnP identity policy.  A full PCI hardware ID is
    // required, NvAPI's combined device/vendor ID layouts are normalized, and
    // a known BDF must match.  Missing/partial/ambiguous identities fail closed
    // for PnP recovery authorization without affecting hardware-write policy.
    {
        SelectedGpuPciHardwareId parsed = {};
        const wchar_t* exact =
            L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C&REV_A1";
        if (!selected_gpu_pnp_parse_hardware_id(exact, &parsed)) return 500;
        if (parsed.vendorId != 0x10DEu || parsed.deviceId != 0x2684u ||
            parsed.subsystemId != 0x17AA3A5Cu || parsed.revisionId != 0xA1u) return 501;
        if (!selected_gpu_pnp_parse_hardware_id(
                L"pci\\ven_10de&dev_2684&subsys_17aa3a5c&rev_a1",
                &parsed)) return 502;
        if (selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C", &parsed)) return 503;
        if (selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_10DE&DEV_02684&SUBSYS_17AA3A5C&REV_A1",
                &parsed)) return 504;

        GpuAdapterInfo target = {};
        target.valid = 1;
        target.pciInfoValid = 1;
        target.deviceId = 0x268410DEu;
        target.subSystemId = 0x17AA3A5Cu;
        target.pciRevisionId = 0xA1u;
        if (!selected_gpu_pnp_parse_hardware_id(exact, &parsed) ||
            !selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 505;
        target.deviceId = 0x10DE2684u;
        if (!selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 506;
        target.deviceId = 0x2684u;
        if (!selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 507;
        target.deviceId = 0x268510DEu;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 508;
        target.deviceId = 0x268410DEu;
        target.subSystemId ^= 1u;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 509;
        target.subSystemId ^= 1u;
        target.pciRevisionId = 0xA2u;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &parsed)) return 510;
        target.pciRevisionId = 0xA1u;
        SelectedGpuPciHardwareId wrongVendor = {};
        if (!selected_gpu_pnp_parse_hardware_id(
                L"PCI\\VEN_1234&DEV_2684&SUBSYS_17AA3A5C&REV_A1",
                &wrongVendor)) return 520;
        if (selected_gpu_pnp_hardware_id_matches_target(
                &target, &wrongVendor)) return 521;

        const wchar_t ids[] =
            L"PCI\\VEN_10DE&DEV_2684\0"
            L"PCI\\VEN_10DE&DEV_2684&SUBSYS_17AA3A5C&REV_A1\0\0";
        if (!selected_gpu_pnp_hardware_id_list_matches_target(
                &target, ids, ARRAY_COUNT(ids))) return 511;
        // GpuAdapterInfo has no historical BDF-valid bit. An all-zero location
        // must fall back to the unique full hardware ID rather than pretending
        // that 0000:00:00.0 was independently corroborated.
        if (selected_gpu_pnp_target_has_bdf(&target)) return 565;
        target.pciBus = 7;
        target.pciDevice = 0;
        target.pciFunction = 0;
        if (!selected_gpu_pnp_target_has_bdf(&target)) return 512;
        if (!selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 0, 0)) return 513;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 8, 0, 0)) return 514;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 1, 0)) return 566;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, true, 7, 0, 1)) return 567;
        if (selected_gpu_pnp_bdf_matches_target(
                &target, false, 0, 0, 0)) return 515;
        target.pciDomain = 1;
        if (selected_gpu_pnp_target_has_bdf(&target)) return 516;
        if (selected_gpu_pnp_resolve_match_count(0) !=
                SELECTED_GPU_PNP_MATCH_NONE) return 517;
        if (selected_gpu_pnp_resolve_match_count(1) !=
                SELECTED_GPU_PNP_MATCH_UNIQUE) return 518;
        if (selected_gpu_pnp_resolve_match_count(2) !=
                SELECTED_GPU_PNP_MATCH_AMBIGUOUS) return 519;
    }

    // Automatic-restore policy: a configured logon profile may be applied at
    // the user's Windows logon unless safety has been latched off. Standby
    // restores current active intent immediately (unless locked out); driver
    // recovery is deliberately stricter and needs a successful apply that
    // survived the full 10-minute proving period. These boundary cases
    // must stay independent of scheduler timing and never become a continuous
    // curve-drift correction policy.
    {
        if (!should_auto_apply_logon_profile(true, false)) return 400;
        if (should_auto_apply_logon_profile(false, false)) return 401;
        if (should_auto_apply_logon_profile(true, true)) return 402;
        if (!should_auto_restore_after_standby_resume(true, false)) return 403;
        if (should_auto_restore_after_standby_resume(false, false)) return 404;
        if (should_auto_restore_after_standby_resume(true, true)) return 405;
        if (should_auto_restore_after_driver_event(false, AUTO_RESTORE_STABILITY_WINDOW_MS, false)) return 406;
        if (should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS - 1, false)) return 407;
        if (!should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS, false)) return 408;
        if (should_auto_restore_after_driver_event(true, AUTO_RESTORE_STABILITY_WINDOW_MS + 1, true)) return 409;
        if (service_should_preserve_proof_after_standby(
                true, AUTO_RESTORE_STABILITY_WINDOW_MS - 1,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 626;
        if (!service_should_preserve_proof_after_standby(
                true, AUTO_RESTORE_STABILITY_WINDOW_MS,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 627;
        if (service_should_preserve_proof_after_standby(
                false, AUTO_RESTORE_STABILITY_WINDOW_MS + 1,
                AUTO_RESTORE_STABILITY_WINDOW_MS)) return 628;
    }

    // Fake unbiased clock: prove the exact 9:59/10:00 boundary without sleep.
    // "Wall time spent asleep" is intentionally absent; unchanged awake ticks
    // leave the proof age unchanged.  Old/cross-boot/ambiguous stamps fail closed.
    {
        const ServiceBootIdentity boot = { 0x12345678ULL, 0x9abcdef0ULL };
        const ServiceBootIdentity anotherBoot = { 0x12345678ULL, 0x9abcdef1ULL };
        const ServiceBootIdentity anotherHighBoot = { 0x12345679ULL, 0x9abcdef0ULL };
        const ServiceBootIdentity invalidBoot = {};
        const uint64_t appliedAwake = 100000000ULL;
        ServiceOcApplyProofStamp stamp = {};
        stamp.magic = SERVICE_OC_APPLY_STAMP_MAGIC;
        stamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
        stamp.size = sizeof(stamp);
        stamp.bootIdentity = boot;
        stamp.awakeTime100ns = appliedAwake;
        uint64_t ageMs = 0;
        if (!service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 599000ULL * 10000ULL, &ageMs) || ageMs != 599000ULL) return 432;
        if (should_auto_restore_after_driver_event(true, ageMs, false)) return 433;
        if (!service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs) || ageMs != 600000ULL) return 434;
        if (!should_auto_restore_after_driver_event(true, ageMs, false)) return 435;

        // Ten minutes of wall-clock standby with no awake-tick progress proves nothing.
        if (!service_compute_proof_age_ms(&stamp, boot, appliedAwake, &ageMs) || ageMs != 0) return 436;
        if (should_auto_restore_after_driver_event(true, ageMs, false)) return 437;
        if (service_compute_proof_age_ms(&stamp, anotherBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 438;
        if (service_compute_proof_age_ms(&stamp, anotherHighBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 624;
        if (service_compute_proof_age_ms(&stamp, invalidBoot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 625;
        stamp.version = 1;
        if (service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 439;
        stamp.version = SERVICE_OC_APPLY_STAMP_VERSION;
        stamp.reserved = 1;
        if (service_compute_proof_age_ms(&stamp, boot,
                appliedAwake + 600000ULL * 10000ULL, &ageMs)) return 440;
        stamp.reserved = 0;

        // Every successful reapply starts a fresh proof, rather than inheriting
        // the old application age.
        stamp.awakeTime100ns = appliedAwake + 600000ULL * 10000ULL;
        if (!service_compute_proof_age_ms(&stamp, boot, stamp.awakeTime100ns, &ageMs) || ageMs != 0) return 441;

        ServiceRecoveryClockEntry history[] = {
            { boot, stamp.awakeTime100ns - 10000ULL },
            { boot, stamp.awakeTime100ns - 20000ULL },
            { boot, stamp.awakeTime100ns - 30000ULL },
            { anotherBoot, stamp.awakeTime100ns },
        };
        if (service_count_recent_recovery_clock_entries(history, ARRAY_COUNT(history),
                boot, stamp.awakeTime100ns, 300000ULL) != 3) return 442;
        // No awake-time progress leaves the persistent spam count intact.
        if (service_count_recent_recovery_clock_entries(history, ARRAY_COUNT(history),
                boot, stamp.awakeTime100ns, 300000ULL) != 3) return 443;
        ServiceRecoveryEvidenceKey evidence[] = {
            { 0xAAULL, 0x11ULL },
            { 0xBBULL, 0x22ULL },
        };
        ServiceRecoveryEvidenceKey corroborating = { 0xAAULL, 0x11ULL };
        ServiceRecoveryEvidenceKey distinct = { 0xAAULL, 0x12ULL };
        if (!service_recovery_evidence_already_recorded(
                evidence, ARRAY_COUNT(evidence), corroborating)) return 456;
        if (service_recovery_evidence_already_recorded(
                evidence, ARRAY_COUNT(evidence), distinct)) return 457;

        // SCM does not guarantee a valid PID while STOP_PENDING. A cleanly
        // exited, pinned parent may therefore transition through pid=0 or a
        // stale value without being mistaken for a different generation.
        if (service_classify_controlled_recovery_scm_stop_state(
                true, false, 0, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED) return 650;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 0, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 651;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 9999, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 652;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, false, 20184, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_WAIT_FOR_STOPPED) return 653;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, false, 9999, 20184) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 654;
        if (service_classify_controlled_recovery_scm_stop_state(
                false, true, 0, 0) !=
                SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) return 655;
    }

    // Deterministic lifecycle reducer tests.  Hardware writes are represented
    // only by incrementing fakeWrites when the reducer issues an authorization;
    // there are no clocks, sleeps, threads, GPU calls, or timing assumptions.
    {
        auto identity = [](gc_u32 session, gc_u64 auth, const char* sid) {
            ServiceLifecycleIdentity value = {};
            value.valid = 1;
            value.sessionId = session;
            value.authenticationId = auth;
            StringCchCopyA(value.sid, ARRAY_COUNT(value.sid), sid);
            return value;
        };
        ServiceLifecycleIdentity loginA = identity(7, 1001, "S-1-5-21-test");
        ServiceLifecycleIdentity loginB = identity(7, 1002, "S-1-5-21-test");
        int fakeWrites = 0;

        // Task-only profile-2 login: readiness may signal repeatedly, but the
        // single write transition is authorized exactly once.
        ServiceLifecycleState state = {};
        ServiceLifecycleEvent event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        ServiceLifecycleDecision decision = service_lifecycle_reduce(&state, &event);
        if (!state.logonPending || !decision.wakeWorker || !decision.attemptLogonPrerequisites) return 410;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.attemptLogonPrerequisites) return 411;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.attemptLogonPrerequisites || fakeWrites != 0) return 412;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.authorizeLogonWrite) ++fakeWrites;
        if (fakeWrites != 1) return 413;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.authorizeLogonWrite) ++fakeWrites;
        if (fakeWrites != 1) return 414;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 1;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_APPLIED || state.logonPending) return 415;

        // A racing WTS event for the same login is coalesced.  Reusing session
        // ID and SID with a new authentication LUID is a distinct login.
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_WTS_LOGON;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.coalesced || state.logonPending || fakeWrites != 1) return 416;
        event.identity = loginB;
        decision = service_lifecycle_reduce(&state, &event);
        if (decision.coalesced || !state.logonPending || state.logonGeneration != 2) return 417;
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.coalesced || state.logonGeneration != 2) return 418;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGOFF;
        decision = service_lifecycle_reduce(&state, &event);
        if (!decision.cancelled || state.logonPending ||
            decision.result != SERVICE_LIFECYCLE_RESULT_CANCELLED_LOGOFF) return 419;

        // A pending login can be superseded or locked out without authorizing a
        // write.  A zero-initialized/ordinary-start state is inert.
        ServiceLifecycleState inert = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_NONE;
        decision = service_lifecycle_reduce(&inert, &event);
        if (decision.authorizeLogonWrite || decision.authorizeStandbyWrite ||
            decision.authorizeDriverWrite || decision.wakeWorker) return 420;
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        service_lifecycle_reduce(&inert, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE;
        decision = service_lifecycle_reduce(&inert, &event);
        if (!decision.cancelled || inert.logonPending) return 421;
        event.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
        service_lifecycle_reduce(&inert, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginB;
        decision = service_lifecycle_reduce(&inert, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_LOCKED_OUT || inert.logonPending) return 422;

        // A failure before the hardware boundary is a prerequisite failure: it
        // releases the one-write authorization and keeps intent pending. Once
        // writeAttempted is true, success or failure is terminal and no later
        // readiness signal may replay the write.
        ServiceLifecycleState logonFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF;
        event.identity = loginA;
        service_lifecycle_reduce(&logonFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.authorizeLogonWrite) return 580;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !logonFailure.logonPending || logonFailure.logonWriteIssued) return 581;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.wakeWorker || !decision.attemptLogonPrerequisites) return 582;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED;
        event.identity = loginA;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (!decision.authorizeLogonWrite) return 583;
        event.type = SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            logonFailure.logonPending) return 584;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL;
        decision = service_lifecycle_reduce(&logonFailure, &event);
        if (decision.wakeWorker || decision.attemptLogonPrerequisites ||
            decision.authorizeLogonWrite) return 585;

        ServiceLifecycleState standbyFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&standbyFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        service_lifecycle_reduce(&standbyFailure, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (!decision.authorizeStandbyWrite) return 586;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !standbyFailure.standbyPending || standbyFailure.standbyWriteIssued) return 587;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (!decision.authorizeStandbyWrite) return 588;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&standbyFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            standbyFailure.standbyPending) return 589;

        ServiceLifecycleState driverFailure = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        event.driverProofReady = 1;
        service_lifecycle_reduce(&driverFailure, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (!decision.authorizeDriverWrite) return 590;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 0;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY ||
            !driverFailure.driverPending || driverFailure.driverWriteIssued) return 591;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        event.driverProofReady = 1;
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (!decision.authorizeDriverWrite) return 592;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED;
        event.success = 0;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_FAILED ||
            driverFailure.driverPending) return 593;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL;
        event.driverProofReady = 1;
        decision = service_lifecycle_reduce(&driverFailure, &event);
        if (decision.wakeWorker || decision.authorizeDriverWrite) return 594;

        // One full-intent authorization per suspend generation, with no driver
        // proof input. Duplicate resume/write-start notifications coalesce.
        ServiceLifecycleState power = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&power, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        decision = service_lifecycle_reduce(&power, &event);
        if (!decision.wakeWorker || !power.standbyPending) return 423;
        // The later Windows resume notification is a real readiness cue when
        // the first serialized probe was too early. It wakes the same pending
        // generation but must not create or authorize another write.
        decision = service_lifecycle_reduce(&power, &event);
        if (!decision.wakeWorker || !decision.coalesced ||
            !power.standbyPending || power.suspendGeneration != 1) return 424;
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED;
        decision = service_lifecycle_reduce(&power, &event);
        int standbyWrites = decision.authorizeStandbyWrite ? 1 : 0;
        decision = service_lifecycle_reduce(&power, &event);
        if (decision.authorizeStandbyWrite) ++standbyWrites;
        if (standbyWrites != 1) return 425;
        event.type = SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED;
        event.success = 1;
        event.writeAttempted = 1;
        decision = service_lifecycle_reduce(&power, &event);
        if (decision.result != SERVICE_LIFECYCLE_RESULT_APPLIED || power.standbyPending) return 426;

        // Sparse fan/memory/power requests do not own the VF/lock domain and
        // therefore must never reset an existing hard clock lock. A reset,
        // curve, GPU-offset, or explicit lock request intentionally replaces it.
        DesiredSettings lockDomain = {};
        if (service_request_replaces_lock_domain(&lockDomain)) return 570;
        lockDomain.hasFan = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 571;
        lockDomain = {};
        lockDomain.hasMemOffset = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 572;
        lockDomain = {};
        lockDomain.hasPowerLimit = 1;
        if (service_request_replaces_lock_domain(&lockDomain)) return 573;
        lockDomain = {};
        lockDomain.resetOcBeforeApply = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 574;
        lockDomain = {};
        lockDomain.hasGpuOffset = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 575;
        lockDomain = {};
        lockDomain.hasCurvePoint[77] = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 576;
        lockDomain = {};
        lockDomain.hasLock = 1;
        if (!service_request_replaces_lock_domain(&lockDomain)) return 577;

        // Standby restoration copies the complete in-memory intent.  Sparse
        // curve-only, lock-only, fan-only and combined requests retain every
        // owned field.  Reset-to-stock is added only for owned VF/GPU-offset
        // policy; a lock-only or fan-only restore must not reset unrelated OC.
        DesiredSettings restoreCases[4] = {};
        restoreCases[0].hasGpuOffset = 1;
        restoreCases[0].gpuOffsetMHz = 125;
        restoreCases[0].hasMemOffset = 1;
        restoreCases[0].memOffsetMHz = 700;
        restoreCases[0].hasPowerLimit = 1;
        restoreCases[0].powerLimitPct = 92;
        restoreCases[0].hasCurvePoint[77] = 1;
        restoreCases[0].curvePointMHz[77] = 2460;
        restoreCases[0].hasLock = 1;
        restoreCases[0].lockCi = 77;
        restoreCases[0].lockMHz = 2460;
        restoreCases[0].hasFan = 1;
        restoreCases[0].fanMode = FAN_MODE_FIXED;
        restoreCases[0].fanPercent = 61;
        restoreCases[1].hasCurvePoint[88] = 1;
        restoreCases[1].curvePointMHz[88] = 2515;
        restoreCases[2].hasLock = 1;
        restoreCases[2].lockCi = 91;
        restoreCases[2].lockMHz = 2550;
        restoreCases[2].lockMode = LOCK_MODE_HARD;
        restoreCases[3].hasFan = 1;
        restoreCases[3].fanMode = FAN_MODE_FIXED;
        restoreCases[3].fanPercent = 55;
        const bool expectReset[4] = { true, true, false, false };
        for (int restoreCase = 0; restoreCase < 4; ++restoreCase) {
            DesiredSettings request = {};
            if (!service_build_full_restore_request(&restoreCases[restoreCase], &request) ||
                request.resetOcBeforeApply != expectReset[restoreCase]) {
                return 480 + restoreCase;
            }
            request.resetOcBeforeApply = 0;
            if (memcmp(&request, &restoreCases[restoreCase], sizeof(request)) != 0) {
                return 484 + restoreCase;
            }
        }

        // Named-profile transitions replace ownership instead of inheriting
        // omitted controls from another account/profile. Hardware cleanup may
        // write defaults for fields Green Curve previously owned, while the
        // new ownership declaration itself remains byte-for-byte unchanged.
        DesiredSettings previousProfile = restoreCases[0];
        DesiredSettings nextFanOnly = {};
        nextFanOnly.hasFan = 1;
        nextFanOnly.fanMode = FAN_MODE_FIXED;
        nextFanOnly.fanPercent = 47;
        DesiredSettings transition = {};
        if (!service_build_profile_transition_request(
                &previousProfile, &nextFanOnly, &transition)) return 530;
        if (!transition.resetOcBeforeApply || !transition.hasGpuOffset ||
            transition.gpuOffsetMHz != 0 || !transition.hasMemOffset ||
            transition.memOffsetMHz != 0 || !transition.hasPowerLimit ||
            transition.powerLimitPct != 100 || !transition.hasFan ||
            transition.fanMode != FAN_MODE_FIXED ||
            transition.fanPercent != 47) return 531;
        if (!nextFanOnly.hasFan || nextFanOnly.hasGpuOffset ||
            nextFanOnly.hasMemOffset || nextFanOnly.hasPowerLimit) return 532;

        DesiredSettings previousFanOnly = nextFanOnly;
        DesiredSettings nextCurveOnly = {};
        nextCurveOnly.hasCurvePoint[90] = 1;
        nextCurveOnly.curvePointMHz[90] = 2505;
        transition = {};
        if (!service_build_profile_transition_request(
                &previousFanOnly, &nextCurveOnly, &transition) ||
            !transition.resetOcBeforeApply || !transition.hasFan ||
            !transition.fanAuto || transition.fanMode != FAN_MODE_AUTO ||
            !transition.hasCurvePoint[90]) return 533;

        DesiredSettings nextLockOnly = {};
        nextLockOnly.hasLock = 1;
        nextLockOnly.lockCi = 91;
        nextLockOnly.lockMHz = 2540;
        transition = {};
        if (!service_build_profile_transition_request(
                nullptr, &nextLockOnly, &transition) ||
            transition.resetOcBeforeApply) return 534;
        transition = {};
        if (!service_build_profile_transition_request(
                &nextLockOnly, &nextFanOnly, &transition) ||
            !transition.resetOcBeforeApply || transition.hasGpuOffset) return 535;

        // Driver recovery waits for explicit proof readiness. It dominates a
        // coincident standby and a duplicate recovery event cannot authorize a
        // second write after WRITE_STARTED.
        ServiceLifecycleState recovery = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_SUSPEND;
        service_lifecycle_reduce(&recovery, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_RESUME;
        service_lifecycle_reduce(&recovery, &event);
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        event.driverProofReady = 0;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.wakeWorker || !recovery.driverPending || recovery.standbyPending) return 427;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) return 428;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL;
        event.driverProofReady = 1;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (!decision.wakeWorker) return 429;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) return 488; // old/non-nonce process
        event.controlledRecoveryValidated = 1;
        decision = service_lifecycle_reduce(&recovery, &event);
        int driverWrites = decision.authorizeDriverWrite ? 1 : 0;
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY;
        decision = service_lifecycle_reduce(&recovery, &event);
        event.type = SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED;
        decision = service_lifecycle_reduce(&recovery, &event);
        if (decision.authorizeDriverWrite) ++driverWrites;
        if (driverWrites != 1) return 430;

        // Global/null DBT_DEVNODES_CHANGED is read-only and cannot create or
        // authorize restoration work.
        ServiceLifecycleState devnodes = {};
        event = {};
        event.type = SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED;
        decision = service_lifecycle_reduce(&devnodes, &event);
        if (!decision.readOnlyReenumerate || decision.authorizeLogonWrite ||
            decision.authorizeStandbyWrite || decision.authorizeDriverWrite ||
            devnodes.logonPending || devnodes.standbyPending || devnodes.driverPending) return 431;
    }

    // Executable startup-task XML fixtures.  These call the production pure
    // classifier and cover Task Scheduler's omitted defaults, SID principals,
    // quoting, compatible delay/elevation, and broken identity/action state.
    {
        const WCHAR* expectedUser = L"TEST\\User";
        const WCHAR* expectedExe = L"C:\\Program Files\\Green Curve\\greencurve.exe";
        const WCHAR* expectedConfig = L"C:\\Users\\Test User\\config.ini";
        const WCHAR* expectedWorkingDir = L"C:\\Program Files\\Green Curve";
        std::wstring canonical = LR"XML(
<Task>
  <Triggers><LogonTrigger><UserId>TEST\User</UserId></LogonTrigger></Triggers>
  <Principals><Principal><UserId>TEST\User</UserId><LogonType>InteractiveToken</LogonType></Principal></Principals>
  <Settings><StartWhenAvailable>true</StartWhenAvailable><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT3M</ExecutionTimeLimit></Settings>
  <Actions><Exec><Command>C:\Program Files\Green Curve\greencurve.exe</Command><Arguments>--logon-start --config &quot;C:\Users\Test User\config.ini&quot;</Arguments><WorkingDirectory>C:\Program Files\Green Curve</WorkingDirectory></Exec></Actions>
</Task>)XML";
        auto classify = [&](const std::wstring& xml, const WCHAR* user = nullptr) {
            char detail[512] = {};
            return startup_task_definition_classify_xml(xml.c_str(),
                user ? user : expectedUser, expectedExe, expectedConfig,
                expectedWorkingDir, detail, sizeof(detail));
        };
        auto replace_all = [](std::wstring value, const std::wstring& from,
                              const std::wstring& to) {
            size_t pos = 0;
            while ((pos = value.find(from, pos)) != std::wstring::npos) {
                value.replace(pos, from.size(), to);
                pos += to.size();
            }
            return value;
        };
        auto insert_after = [](std::wstring value, const std::wstring& marker,
                               const std::wstring& addition) {
            size_t pos = value.find(marker);
            if (pos != std::wstring::npos) value.insert(pos + marker.size(), addition);
            return value;
        };

        // Enabled and RunLevel are schema defaults and may be omitted or empty.
        {
            char canonicalDetail[512] = {};
            StartupTaskDefinitionClass canonicalClass =
                startup_task_definition_classify_xml(canonical.c_str(), expectedUser,
                    expectedExe, expectedConfig, expectedWorkingDir,
                    canonicalDetail, sizeof(canonicalDetail));
            if (canonicalClass != STARTUP_TASK_DEFINITION_CANONICAL) {
                fprintf(stderr, "canonical startup-task fixture classified %d: %s\n",
                    (int)canonicalClass, canonicalDetail);
                return 444;
            }
        }

        // Mirror the XML emitted by write_startup_task_xml(), including the
        // explicit values Task Scheduler may preserve in its query output. The
        // reported regression was caused by generator/verifier drift around
        // Enabled and LeastPrivilege, so omitted-default fixtures are not enough.
        std::wstring generatedCanonical = LR"XML(
<Task version="1.3" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Author>TEST\User</Author><Description>Notify the Green Curve service of an authenticated user logon.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>TEST\User</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>TEST\User</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT3M</ExecutionTimeLimit><Priority>7</Priority>
  </Settings>
  <Actions Context="Author"><Exec><Command>C:\Program Files\Green Curve\greencurve.exe</Command><WorkingDirectory>C:\Program Files\Green Curve</WorkingDirectory><Arguments>--logon-start --config &quot;C:\Users\Test User\config.ini&quot;</Arguments></Exec></Actions>
</Task>)XML";
        {
            char generatedDetail[512] = {};
            StartupTaskDefinitionClass generatedClass =
                startup_task_definition_classify_xml(generatedCanonical.c_str(),
                    expectedUser, expectedExe, expectedConfig,
                    expectedWorkingDir, generatedDetail,
                    sizeof(generatedDetail));
            if (generatedClass != STARTUP_TASK_DEFINITION_CANONICAL) {
                fprintf(stderr, "generated startup-task fixture classified %d: %s\n",
                    (int)generatedClass, generatedDetail);
                return 600;
            }
        }
        std::wstring emptyDefaults = insert_after(canonical, L"<LogonTrigger>", L"<Enabled/>");
        emptyDefaults = insert_after(emptyDefaults, L"<Principal>", L"<RunLevel/>");
        emptyDefaults = insert_after(emptyDefaults, L"<Settings>", L"<Enabled/>");
        if (classify(emptyDefaults) != STARTUP_TASK_DEFINITION_CANONICAL) return 445;

        std::wstring sidPrincipal = replace_all(canonical, L"TEST\\User", L"S-1-5-18");
        if (classify(sidPrincipal, L"S-1-5-18") != STARTUP_TASK_DEFINITION_CANONICAL) return 446;

        std::wstring delayed = insert_after(canonical, L"<LogonTrigger>", L"<Delay>PT30S</Delay>");
        if (classify(delayed) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 447;
        std::wstring highest = insert_after(canonical, L"<Principal>", L"<RunLevel>HighestAvailable</RunLevel>");
        if (classify(highest) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 448;
        std::wstring oldLimit = replace_all(canonical, L"PT3M", L"PT0S");
        if (classify(oldLimit) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 449;
        std::wstring omittedSafeDefault = replace_all(canonical,
            L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>", L"");
        if (classify(omittedSafeDefault) != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) return 540;

        std::wstring triggerDisabled = insert_after(canonical, L"<LogonTrigger>", L"<Enabled>false</Enabled>");
        if (classify(triggerDisabled) != STARTUP_TASK_DEFINITION_BROKEN) return 450;
        std::wstring taskDisabled = insert_after(canonical, L"<Settings>", L"<Enabled>false</Enabled>");
        if (classify(taskDisabled) != STARTUP_TASK_DEFINITION_BROKEN) return 451;
        std::wstring wrongUser = replace_all(canonical, L"TEST\\User", L"TEST\\Other");
        if (classify(wrongUser) != STARTUP_TASK_DEFINITION_BROKEN) return 452;
        std::wstring staleExe = replace_all(canonical,
            L"C:\\Program Files\\Green Curve\\greencurve.exe",
            L"C:\\Old\\greencurve.exe");
        if (classify(staleExe) != STARTUP_TASK_DEFINITION_BROKEN) return 453;
        std::wstring staleConfig = replace_all(canonical,
            L"C:\\Users\\Test User\\config.ini", L"C:\\Old\\config.ini");
        if (classify(staleConfig) != STARTUP_TASK_DEFINITION_BROKEN) return 454;
        std::wstring staleAction = insert_after(canonical, L"<Actions>",
            L"<Exec><Command>C:\\Other.exe</Command><Arguments>--bad</Arguments><WorkingDirectory>C:\\</WorkingDirectory></Exec>");
        if (classify(staleAction) != STARTUP_TASK_DEFINITION_BROKEN) return 455;
        std::wstring extraTrigger = insert_after(canonical, L"<Triggers>",
            L"<BootTrigger><Enabled>true</Enabled></BootTrigger>");
        if (classify(extraTrigger) != STARTUP_TASK_DEFINITION_BROKEN) return 541;
        std::wstring repeatedTrigger = insert_after(canonical, L"<LogonTrigger>",
            L"<Repetition><Interval>PT1M</Interval></Repetition>");
        if (classify(repeatedTrigger) != STARTUP_TASK_DEFINITION_BROKEN) return 542;
        std::wstring pt1s = replace_all(canonical, L"PT3M", L"PT1S");
        if (classify(pt1s) != STARTUP_TASK_DEFINITION_BROKEN) return 543;
        std::wstring queued = replace_all(canonical, L"IgnoreNew", L"Queue");
        if (classify(queued) != STARTUP_TASK_DEFINITION_BROKEN) return 544;
        std::wstring parallel = replace_all(canonical, L"IgnoreNew", L"Parallel");
        if (classify(parallel) != STARTUP_TASK_DEFINITION_BROKEN) return 545;
        std::wstring stopExisting = replace_all(canonical, L"IgnoreNew", L"StopExisting");
        if (classify(stopExisting) != STARTUP_TASK_DEFINITION_BROKEN) return 546;
        std::wstring batteryGated = replace_all(canonical,
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>",
            L"<DisallowStartIfOnBatteries>true</DisallowStartIfOnBatteries>");
        if (classify(batteryGated) != STARTUP_TASK_DEFINITION_BROKEN) return 547;
        std::wstring idleGated = insert_after(canonical, L"<Settings>",
            L"<RunOnlyIfIdle>true</RunOnlyIfIdle>");
        if (classify(idleGated) != STARTUP_TASK_DEFINITION_BROKEN) return 548;
        std::wstring restartOnFailure = insert_after(canonical, L"<Settings>",
            L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>");
        if (classify(restartOnFailure) != STARTUP_TASK_DEFINITION_BROKEN) return 549;
        std::wstring unavailable = replace_all(canonical,
            L"<StartWhenAvailable>true</StartWhenAvailable>",
            L"<StartWhenAvailable>false</StartWhenAvailable>");
        if (classify(unavailable) != STARTUP_TASK_DEFINITION_BROKEN) return 550;
        std::wstring missingBatteryPolicy = replace_all(canonical,
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>", L"");
        if (classify(missingBatteryPolicy) != STARTUP_TASK_DEFINITION_BROKEN) return 551;
        std::wstring hiddenTask = insert_after(canonical, L"<Settings>",
            L"<Hidden>true</Hidden>");
        if (classify(hiddenTask) != STARTUP_TASK_DEFINITION_BROKEN) return 552;
        std::wstring wrongPriority = insert_after(canonical, L"<Settings>",
            L"<Priority>4</Priority>");
        if (classify(wrongPriority) != STARTUP_TASK_DEFINITION_BROKEN) return 553;
        std::wstring noHardTerminate = insert_after(canonical, L"<Settings>",
            L"<AllowHardTerminate>false</AllowHardTerminate>");
        if (classify(noHardTerminate) != STARTUP_TASK_DEFINITION_BROKEN) return 554;
    }

    // The mutually-exclusive logon selection is one locked transaction.
    // Injected commit failure must leave BOTH old keys intact; successful
    // commit must update both while preserving unrelated [profiles] keys.
    {
        DeleteFileA(argv[1]);
        if (!set_config_int(argv[1], "profiles", "selected_slot", 4)) return 458;
        if (!set_config_int(argv[1], "profiles", "applied_slot", 3)) return 459;
        if (!set_config_int(argv[1], "profiles", "logon_slot", 1)) return 460;
        if (!set_config_int(argv[1], "profiles", "logon_shared_slot", 0)) return 461;

        auto failCommit = +[](const char*, const char*, void*, char* err, size_t errSize) -> bool {
            set_message(err, errSize, "injected config transaction failure");
            return false;
        };
        char txErr[256] = {};
        if (update_logon_profile_selection_transaction(argv[1], 0, 3,
                failCommit, nullptr, txErr, sizeof(txErr))) return 462;
        if (get_config_int(argv[1], "profiles", "logon_slot", -1) != 1 ||
            get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 0) return 463;

        auto commitWholeText = +[](const char* path, const char* text, void*,
                                   char* err, size_t errSize) -> bool {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                set_message(err, errSize, "test commit create failed");
                return false;
            }
            DWORD length = (DWORD)strlen(text);
            DWORD written = 0;
            bool ok = WriteFile(file, text, length, &written, nullptr) != FALSE &&
                written == length && FlushFileBuffers(file) != FALSE;
            CloseHandle(file);
            if (!ok) set_message(err, errSize, "test commit write failed");
            return ok;
        };
        if (!update_logon_profile_selection_transaction(argv[1], 0, 3,
                commitWholeText, nullptr, txErr, sizeof(txErr))) return 464;
        if (get_config_int(argv[1], "profiles", "logon_slot", -1) != 0 ||
            get_config_int(argv[1], "profiles", "logon_shared_slot", -1) != 3) return 465;
        if (get_config_int(argv[1], "profiles", "selected_slot", -1) != 4 ||
            get_config_int(argv[1], "profiles", "applied_slot", -1) != 3) return 466;
        if (update_logon_profile_selection_transaction(argv[1], 2, 3,
                commitWholeText, nullptr, txErr, sizeof(txErr))) return 467;

        // Async combo synchronization uses tagged item data, not list index.
        if (logon_profile_selection_item_data(2, 0) != 2) return 468;
        if (logon_profile_selection_item_data(0, 3) != (LOGON_COMBO_SHARED_FLAG | 3)) return 469;
        if (logon_profile_selection_item_data(2, 3) != (LOGON_COMBO_SHARED_FLAG | 3)) return 470;
        if (logon_profile_selection_item_data(-1, 99) != 0) return 471;

        // Applied ownership is service metadata, never live VF-MHz equality.
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_USER_SLOT, 2) != 2) return 472;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_SHARED_SLOT, 2) != 0) return 473;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_MACHINE_SLOT, 2) != 0) return 474;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_AD_HOC, 2) != 0) return 475;
        if (applied_user_slot_from_service_profile(SERVICE_PROFILE_SOURCE_USER_SLOT, 0) != 0) return 476;

        // Win32 treats INI section names case-insensitively.  The production
        // direct-file section replacer uses this helper, so a hand-edited
        // [Profiles] header cannot survive beside a new [profiles] copy.
        if (!config_section_header_matches_ascii("[profiles]", "profiles")) return 523;
        if (!config_section_header_matches_ascii("[Profiles]", "profiles")) return 524;
        if (!config_section_header_matches_ascii("[PROFILES]", "profiles")) return 525;
        if (config_section_header_matches_ascii("[profiles_old]", "profiles")) return 526;
        if (config_section_header_matches_ascii("[profile]", "profiles")) return 527;
    }

    // Stable selected-GPU identity config parsing is coherent and bounded.
    {
        DeleteFileA(argv[1]);
        if (!set_config_int(argv[1], "gpu", "selected_index", 1) ||
            !set_config_int(argv[1], "gpu", "selected_identity_version", 1) ||
            !set_config_string(argv[1], "gpu", "selected_device_id", "00001234") ||
            !set_config_string(argv[1], "gpu", "selected_subsystem_id", "00005678") ||
            !set_config_string(argv[1], "gpu", "selected_revision_id", "00000001") ||
            !set_config_string(argv[1], "gpu", "selected_ext_device_id", "00000002") ||
            !set_config_int(argv[1], "gpu", "selected_bdf_valid", 1) ||
            !set_config_int(argv[1], "gpu", "selected_pci_domain", 0) ||
            !set_config_int(argv[1], "gpu", "selected_pci_bus", 9) ||
            !set_config_int(argv[1], "gpu", "selected_pci_device", 0) ||
            !set_config_int(argv[1], "gpu", "selected_pci_function", 0)) return 609;
        ConfiguredGpuSelection configured = {};
        char configErr[128] = {};
        if (!load_configured_gpu_selection(argv[1], &configured,
                configErr, sizeof(configErr)) ||
            !configured.stableIdentityPresent || configured.legacyIndex != 1 ||
            configured.identity.deviceId != 0x1234 ||
            configured.identity.pciBus != 9) return 610;
        if (!set_config_string(argv[1], "gpu", "selected_device_id", "-1") ||
            load_configured_gpu_selection(argv[1], &configured,
                configErr, sizeof(configErr))) return 611;
        DeleteFileA(argv[1]);
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

    // F-AUTO-PROFILE: the auto-profile rule resolver is a pure, ordered,
    // first-match-wins decision.  These guard the matching contract (exe /
    // title / class / fullscreen, require_focus, default fallback) the whole
    // feature depends on.
    {
        // Case-insensitive substring helper.
        if (!auto_profile_text_contains_ci("Google Chrome", "chrome")) return 220;
        if (auto_profile_text_contains_ci("Google Chrome", "firefox")) return 221;
        if (auto_profile_text_contains_ci("abc", "")) return 222;   // empty pattern never matches
        if (auto_profile_text_contains_ci(nullptr, "x")) return 223;

        // Match-type name round-trip.
        if (auto_profile_match_type_from_name("exe") != AUTO_MATCH_EXE) return 224;
        if (auto_profile_match_type_from_name("fullscreen") != AUTO_MATCH_FULLSCREEN) return 225;
        if (auto_profile_match_type_from_name("bogus") != AUTO_MATCH_NONE) return 226;
        if (strcmp(auto_profile_match_type_name(AUTO_MATCH_TITLE), "title") != 0) return 227;

        AutoProfileConfig cfg = {};
        auto_profile_config_set_defaults(&cfg);
        cfg.enabled = true;
        cfg.defaultSlot = 1;
        cfg.ruleCount = 3;
        cfg.rules[0] = { AUTO_MATCH_EXE, "game.exe", true, 2 };
        cfg.rules[1] = { AUTO_MATCH_TITLE, "YouTube", true, 3 };
        cfg.rules[2] = { AUTO_MATCH_FULLSCREEN, "", true, 4 };

        ForegroundInfo fg = {};
        fg.valid = true;
        ProcessPresence pres = {};

        // exe focus match (case-insensitive).
        StringCchCopyA(fg.exeName, sizeof(fg.exeName), "GAME.EXE");
        StringCchCopyA(fg.title, sizeof(fg.title), "loading");
        fg.isFullscreen = false;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 2) return 228;

        // title match when exe does not match.
        StringCchCopyA(fg.exeName, sizeof(fg.exeName), "chrome.exe");
        StringCchCopyA(fg.title, sizeof(fg.title), "Cats - YouTube");
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 3) return 229;

        // fullscreen fallback rule.
        StringCchCopyA(fg.exeName, sizeof(fg.exeName), "someapp.exe");
        StringCchCopyA(fg.title, sizeof(fg.title), "no keyword");
        fg.isFullscreen = true;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 4) return 230;

        // first-match-wins: exe rule outranks the later title + fullscreen rules.
        StringCchCopyA(fg.exeName, sizeof(fg.exeName), "game.exe");
        StringCchCopyA(fg.title, sizeof(fg.title), "YouTube");
        fg.isFullscreen = true;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 2) return 231;

        // no match → default slot.
        StringCchCopyA(fg.exeName, sizeof(fg.exeName), "notepad.exe");
        StringCchCopyA(fg.title, sizeof(fg.title), "Untitled");
        fg.isFullscreen = false;
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 1) return 232;

        // require_focus honored: a running-but-not-foreground exe does NOT match
        // a focus-required rule.
        pres.rulePresent[0] = true;   // pretend game.exe is running in background
        if (resolve_auto_profile_slot(&cfg, &fg, &pres) != 1) return 233;

        // focus-optional exe rule matches on presence alone.
        AutoProfileConfig bg = {};
        auto_profile_config_set_defaults(&bg);
        bg.enabled = true;
        bg.defaultSlot = 1;
        bg.ruleCount = 1;
        bg.rules[0] = { AUTO_MATCH_EXE, "bg.exe", false, 5 };
        ForegroundInfo other = {};
        other.valid = true;
        StringCchCopyA(other.exeName, sizeof(other.exeName), "explorer.exe");
        ProcessPresence bgPres = {};
        bgPres.rulePresent[0] = true;
        if (resolve_auto_profile_slot(&bg, &other, &bgPres) != 5) return 234;
        bgPres.rulePresent[0] = false;
        if (resolve_auto_profile_slot(&bg, &other, &bgPres) != 1) return 235;
    }

    // F-AUTO-PROFILE: controller state machine — coalescing, cooldown, and the
    // manual-pin hotkey semantics (same slot twice → back to auto).
    {
        AutoProfileConfig cfg = {};
        auto_profile_config_set_defaults(&cfg);
        cfg.enabled = true;
        cfg.defaultSlot = 1;
        cfg.switchDebounceMs = 800;
        cfg.minSwitchIntervalMs = 4000;

        AutoProfileController c = {};
        ap_controller_init(&c, &cfg);
        c.appliedSlot = 1;   // assume default is applied

        // Coalescing: A(1)->B(2)->A(1) within debounce yields NO switch to B.
        AutoProfileAction a = ap_on_target_resolved(&c, 2, 0, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 236;
        a = ap_on_target_resolved(&c, 1, 200, false);
        if (a.kind != AP_ACTION_NONE) return 237;
        a = ap_on_debounce_fire(&c, 1, 800, false);
        if (a.kind != AP_ACTION_NONE || c.appliedSlot != 1) return 238;

        // Sustained B: one apply after debounce.
        a = ap_on_target_resolved(&c, 2, 1000, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 239;
        a = ap_on_debounce_fire(&c, 2, 1800, false);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 2) return 240;
        ap_on_applied(&c, 2, 1800);
        if (c.appliedSlot != 2) return 241;

        // Cooldown: a switch to 3 shortly after must defer until minInterval.
        a = ap_on_target_resolved(&c, 3, 1900, false);
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 242;
        a = ap_on_debounce_fire(&c, 3, 2700, false);   // 2700-1800=900 < 4000
        if (a.kind != AP_ACTION_ARM_DEBOUNCE) return 243;
        a = ap_on_debounce_fire(&c, 3, 5800, false);   // 5800-1800=4000 >= 4000
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 3) return 244;
        ap_on_applied(&c, 3, 5800);

        // Suppression (main window open) → auto does not drive.
        a = ap_on_target_resolved(&c, 1, 6000, true);
        if (a.kind != AP_ACTION_NONE) return 245;

        // Manual pin via hotkey.
        AutoProfileController m = {};
        ap_controller_init(&m, &cfg);
        m.appliedSlot = 1;
        a = ap_on_hotkey(&m, 3);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 3 || m.mode != AP_MODE_MANUAL) return 246;
        ap_on_applied(&m, 3, 100);
        // While pinned, auto does not drive.
        a = ap_on_target_resolved(&m, 2, 200, false);
        if (a.kind != AP_ACTION_NONE) return 247;
        // Same slot again → back to auto.
        a = ap_on_hotkey(&m, 3);
        if (a.kind != AP_ACTION_RESUME_AUTO || m.mode != AP_MODE_AUTO) return 248;
        // Different slot pins that slot.
        a = ap_on_hotkey(&m, 2);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 2 || m.mode != AP_MODE_MANUAL || m.pinnedSlot != 2) return 249;

        // enter_manual_custom suspends auto.
        AutoProfileController mc = {};
        ap_controller_init(&mc, &cfg);
        ap_enter_manual_custom(&mc);
        if (ap_controller_is_driving(&mc, false)) return 250;

        // Master toggle: disable reverts to default; enable resumes auto.
        AutoProfileController t = {};
        ap_controller_init(&t, &cfg);
        t.appliedSlot = 2;
        a = ap_set_enabled(&t, false);
        if (a.kind != AP_ACTION_APPLY_SLOT || a.slot != 1) return 251;
        a = ap_set_enabled(&t, true);
        if (a.kind != AP_ACTION_RESUME_AUTO || !t.autoEnabled || t.mode != AP_MODE_AUTO) return 252;
    }

    // F-AUTO-PROFILE: auto-profile config INI round-trips through the shared
    // get/set_config_* helpers (needs the argv[1] temp INI).
    {
        DeleteFileA(argv[1]);
        AutoProfileConfig w = {};
        auto_profile_config_set_defaults(&w);
        w.enabled = true;
        w.defaultSlot = 2;
        w.switchDebounceMs = 500;
        w.minSwitchIntervalMs = 5000;
        w.suppressWhenWindowOpen = false;
        w.ruleCount = 2;
        w.rules[0] = { AUTO_MATCH_EXE, "game.exe", true, 3 };
        w.rules[1] = { AUTO_MATCH_TITLE, "YouTube", true, 4 };
        char hotkeys[CONFIG_NUM_SLOTS + 1][64] = {};
        StringCchCopyA(hotkeys[3], ARRAY_COUNT(hotkeys[3]), "ctrl+alt+f3");
        if (!auto_profile_config_save(argv[1], &w, hotkeys)) return 256;

        AutoProfileConfig r = {};
        auto_profile_config_load(argv[1], &r);
        if (!r.enabled || r.defaultSlot != 2) return 257;
        if (r.switchDebounceMs != 500 || r.minSwitchIntervalMs != 5000 || r.suppressWhenWindowOpen) return 258;
        if (r.ruleCount != 2 ||
            r.rules[0].matchType != AUTO_MATCH_EXE || strcmp(r.rules[0].pattern, "game.exe") != 0 ||
            !r.rules[0].requireFocus || r.rules[0].slot != 3 ||
            r.rules[1].matchType != AUTO_MATCH_TITLE || strcmp(r.rules[1].pattern, "YouTube") != 0 ||
            r.rules[1].slot != 4) return 259;
        char hotkeyReadback[64] = {};
        if (!get_config_string(argv[1], "hotkeys", "slot3", "",
                hotkeyReadback, ARRAY_COUNT(hotkeyReadback)) ||
            strcmp(hotkeyReadback, "ctrl+alt+f3") != 0) return 607;
        if (config_section_has_keys(argv[1], "auto_rule3")) return 608;
        DeleteFileA(argv[1]);
    }

    // F-AUTO-PROFILE: per-slot hotkey string parse/format round-trip + rejection.
    {
        HotkeyBinding b = {};
        if (!hotkey_parse("ctrl+alt+f2", &b)) return 260;
        if (b.vk != VK_F2 || b.mods != (MOD_CONTROL | MOD_ALT)) return 261;
        char text[64] = {};
        if (!hotkey_format(&b, text, sizeof(text)) || strcmp(text, "ctrl+alt+f2") != 0) return 262;

        HotkeyBinding b2 = {};
        if (!hotkey_parse("CTRL+SHIFT+A", &b2)) return 263;   // case-insensitive
        if (b2.vk != 'A' || b2.mods != (MOD_CONTROL | MOD_SHIFT)) return 264;

        HotkeyBinding b3 = {};
        if (hotkey_parse("ctrl+alt", &b3)) return 265;        // no key
        if (hotkey_parse("ctrl+bogus", &b3)) return 266;      // unknown key token
        // A bare key parses (mods==0); the dialog is what rejects modifier-less binds.
        HotkeyBinding b4 = {};
        if (!hotkey_parse("f5", &b4) || b4.mods != 0 || b4.vk != VK_F5) return 267;
    }

    // Linux daemon state records reject corruption, truncation/version drift,
    // and invalid state before any startup replay can reach hardware.
    {
        GpuAdapterInfo target = {};
        target.valid = true;
        target.pciInfoValid = true;
        target.pciBus = 1;
        target.deviceId = 0x268410deu;
        DesiredSettings desired = {};
        LinuxDaemonStateRecord record = {};
        linux_daemon_record_initialize(&record, LINUX_DAEMON_RECORD_ACTIVE, &target, &desired);
        if (!linux_daemon_record_valid(&record)) return 609;
        LinuxDaemonStateRecord corrupt = record;
        corrupt.desired.gpuOffsetMHz ^= 1;
        if (linux_daemon_record_valid(&corrupt)) return 610;
        corrupt = record; corrupt.size--;
        if (linux_daemon_record_valid(&corrupt)) return 611;
        corrupt = record; corrupt.version++;
        if (linux_daemon_record_valid(&corrupt)) return 612;
        corrupt = record; corrupt.state = 99; corrupt.checksum = linux_daemon_record_checksum(&corrupt);
        if (linux_daemon_record_valid(&corrupt)) return 613;
    }

    // The production Linux mutation engine stops at every possible phase
    // failure, rolls back all attempted (including possibly partial) phases,
    // and exposes rollback uncertainty without publishing success.
    {
        const unsigned int phases[] = {
            LINUX_MUTATION_RESET_BASELINE, LINUX_MUTATION_GPU_OFFSET,
            LINUX_MUTATION_MEM_OFFSET, LINUX_MUTATION_POWER,
            LINUX_MUTATION_CURVE, LINUX_MUTATION_LOCK, LINUX_MUTATION_FAN,
        };
        unsigned int requested = 0;
        for (unsigned int phase : phases) requested |= phase;
        for (unsigned int failIndex = 0; failIndex < 7; ++failIndex) {
            FakeLinuxTransaction fake = {};
            fake.failPhase = phases[failIndex];
            fake.rollbackOk = true;
            LinuxMutationResult result = linux_execute_transaction(
                requested, fake_linux_transaction_step,
                fake_linux_transaction_rollback, &fake);
            if (result.success || !result.rollbackAttempted || !result.rollbackSucceeded) return 619;
            if (result.failedPhases != phases[failIndex] || fake.callCount != failIndex + 1) return 620;
            if (fake.rollbackMask != result.attemptedPhases ||
                (result.completedPhases & phases[failIndex])) return 621;
        }
        FakeLinuxTransaction rollbackFailure = {};
        rollbackFailure.failPhase = LINUX_MUTATION_POWER;
        LinuxMutationResult failed = linux_execute_transaction(
            requested, fake_linux_transaction_step,
            fake_linux_transaction_rollback, &rollbackFailure);
        if (failed.success || failed.rollbackSucceeded || !failed.rollbackAttempted) return 622;
        FakeLinuxTransaction success = {};
        success.rollbackOk = true;
        LinuxMutationResult complete = linux_execute_transaction(
            requested, fake_linux_transaction_step,
            fake_linux_transaction_rollback, &success);
        if (!complete.success || complete.attemptedPhases != requested ||
            complete.completedPhases != requested || complete.failedPhases) return 623;
    }

    // Linux PCI identity remains stable across API enumeration reordering and
    // fails closed for missing, duplicate, or cross-API-mismatched devices.
    {
        GpuAdapterInfo requested = {};
        requested.valid = requested.pciInfoValid = true;
        requested.pciBus = 2; requested.pciDevice = 3;
        requested.deviceId = 0x268410deu; requested.subSystemId = 0x1234u;
        GpuAdapterInfo adapters[2] = {};
        adapters[0] = requested;
        adapters[0].pciBus = 1;
        adapters[1] = requested;
        if (linux_resolve_gpu_identity(&requested, adapters, 2) != 1) return 614;
        GpuAdapterInfo reordered[2] = { adapters[1], adapters[0] };
        if (linux_resolve_gpu_identity(&requested, reordered, 2) != 0) return 615;
        if (linux_resolve_gpu_identity(&requested, adapters, 1) != -1) return 616;
        GpuAdapterInfo duplicate[2] = { requested, requested };
        if (linux_resolve_gpu_identity(&requested, duplicate, 2) != -2) return 617;
        GpuAdapterInfo mismatch = requested;
        mismatch.deviceId = 0x999910deu;
        if (linux_resolve_gpu_identity(&requested, &mismatch, 1) != -1) return 618;
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
            f'-DAPP_VERSION="{APP_VERSION}"',
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


def require_text_count(path, needle, expected, label):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    actual = text.count(needle)
    if actual != expected:
        print(f"Regression source check FAILED (count {actual}, expected {expected}): {label}")
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


def require_order_after(path, anchor, first, second, label):
    """Assert an ordering somewhere after `anchor`.

    Use require_order_in_operation() for safety-critical checks where a later
    function containing the same calls must not satisfy the assertion.
    """
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    anchor_index = text.find(anchor)
    if anchor_index < 0:
        print(f"Regression source check FAILED (order): {label} (anchor missing)")
        sys.exit(1)
    start = anchor_index + len(anchor)
    first_index = text.find(first, start)
    second_index = text.find(second, start)
    if first_index < 0 or second_index < 0 or first_index >= second_index:
        print(f"Regression source check FAILED (order): {label}")
        sys.exit(1)


def _source_operation_region(path, anchor, label):
    """Return the braced C/C++ operation beginning at `anchor`.

    This is deliberately a small lexical scanner rather than a C++ parser: the
    regression checks only need to keep a similarly named apply in a different
    function from satisfying (or defeating) a logon-path check.  Skip quoted
    strings and comments so braces in diagnostics cannot end the region early.
    """
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    anchor_index = text.find(anchor)
    if anchor_index < 0:
        print(f"Regression source check FAILED: {label} (anchor missing)")
        sys.exit(1)
    brace_index = text.find("{", anchor_index + len(anchor))
    if brace_index < 0:
        print(f"Regression source check FAILED: {label} (opening brace missing)")
        sys.exit(1)

    depth = 0
    index = brace_index
    state = "code"
    while index < len(text):
        ch = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "/":
                state = "line-comment"
                index += 1
            elif ch == "/" and nxt == "*":
                state = "block-comment"
                index += 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[anchor_index:index + 1]
        elif state == "string":
            if ch == "\\":
                index += 1
            elif ch == '"':
                state = "code"
        elif state == "char":
            if ch == "\\":
                index += 1
            elif ch == "'":
                state = "code"
        elif state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state == "block-comment" and ch == "*" and nxt == "/":
            state = "code"
            index += 1
        index += 1

    print(f"Regression source check FAILED: {label} (closing brace missing)")
    sys.exit(1)


def require_order_in_operation(path, anchor, first, second, label):
    """Assert `first` then a subsequent `second` in one braced operation."""
    region = _source_operation_region(path, anchor, label)
    first_index = region.find(first)
    second_index = region.find(second,
                               first_index + len(first) if first_index >= 0 else 0)
    if first_index < 0 or second_index < 0:
        print(f"Regression source check FAILED (operation order): {label}")
        sys.exit(1)


def require_text_in_operation(path, anchor, needle, label):
    region = _source_operation_region(path, anchor, label)
    if needle not in region:
        print(f"Regression source check FAILED: {label}")
        sys.exit(1)


def forbid_text_in_operation(path, anchor, needle, label):
    region = _source_operation_region(path, anchor, label)
    if needle in region:
        print(f"Regression source check FAILED (must be absent): {label}")
        sys.exit(1)


def require_app_version_fallback_in_sync():
    """VERSION is the sole release source; headers retain only a dev fallback."""
    expected_define = f'-DAPP_VERSION="{APP_VERSION}"'
    if expected_define not in COMMON_FLAGS:
        print("Regression source check FAILED: VERSION is not injected into compile flags")
        sys.exit(1)
    for rel in ("app_shared.h", "linux_port.h", "linux_daemon.cpp"):
        path = os.path.join(SOURCE_DIR, rel)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        match = re.search(r'#define\s+APP_VERSION\s+"([^"]+)"', text)
        if not match:
            print(f"Regression source check FAILED: APP_VERSION fallback missing in {rel}")
            sys.exit(1)
        if match.group(1) != "dev":
            print(f"Regression source check FAILED: {rel} embeds release version "
                  f"'{match.group(1)}' instead of the neutral dev fallback")
            sys.exit(1)


SOURCE_SIZE_RATCHET = {
    "config_profiles_ui.cpp": 812, "config_profiles.cpp": 1010,
    "entry.cpp": 960, "gpu_backend_apply.cpp": 1333, "gpu_backend.cpp": 975,
    "linux_backend.cpp": 1154,
    "main_fan_runtime.cpp": 934, "main_gpu_front.cpp": 846,
    "main_gpu_state.cpp": 919, "main_runtime_gpu.cpp": 917,
    "main_runtime_nvml.cpp": 1105, "main_runtime_ui.cpp": 809,
    "main_service_persist.cpp": 914, "main_service_pipe.cpp": 841,
    "main_state_sync.cpp": 1215, "ui_main_window.cpp": 1420, "ui_main.cpp": 867,
}


def enforce_source_size_ratchet(soft_limit=800):
    """Reject new oversized modules and growth in grandfathered modules."""
    exempt = {"app_shared.h"}
    oversized = []
    violations = []
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
            ceiling = SOURCE_SIZE_RATCHET.get(name)
            if ceiling is None:
                violations.append(f"new oversized source/{name}: {lines} lines")
            elif lines > ceiling:
                violations.append(f"source/{name}: {lines} lines exceeds ratchet {ceiling}")
    if oversized:
        oversized.sort(reverse=True)
        print(f"NOTE: {len(oversized)} source file(s) exceed the ~{soft_limit}-line guideline (F-MAINT-1):")
        for lines, name in oversized:
            print(f"  {lines:5d}  source/{name}")
    if violations:
        print("Regression source check FAILED: source-size ratchet")
        for violation in violations:
            print(f"  {violation}")
        sys.exit(1)


def run_source_regression_checks():
    enforce_source_size_ratchet()
    require_app_version_fallback_in_sync()
    main_cpp = os.path.join(SOURCE_DIR, "main.cpp")
    entry_cpp = os.path.join(SOURCE_DIR, "entry.cpp")
    diagnostics_cpp = os.path.join(SOURCE_DIR, "main_diagnostics.cpp")
    secure_write_cpp = os.path.join(SOURCE_DIR, "main_secure_write.cpp")
    service_ipc_aggregate_cpp = os.path.join(SOURCE_DIR, "main_service_ipc.cpp")
    service_connection_cpp = os.path.join(SOURCE_DIR, "main_service_connection.cpp")
    service_client_commands_cpp = os.path.join(SOURCE_DIR, "main_service_client_commands.cpp")
    service_admin_client_cpp = os.path.join(SOURCE_DIR, "main_service_admin_client.cpp")
    service_machine_config_cpp = os.path.join(SOURCE_DIR, "main_service_machine_config.cpp")
    service_server_aggregate_cpp = os.path.join(SOURCE_DIR, "main_service_server.cpp")
    service_request_policy_cpp = os.path.join(SOURCE_DIR, "main_service_request_policy.cpp")
    service_pipe_cpp = os.path.join(SOURCE_DIR, "main_service_pipe.cpp")
    service_host_cpp = os.path.join(SOURCE_DIR, "main_service_host.cpp")
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
    main_layout_policy_h = os.path.join(SOURCE_DIR, "main_layout_policy.h")
    ui_main_layout_cpp = os.path.join(SOURCE_DIR, "ui_main_layout.cpp")
    ui_theme_metrics_h = os.path.join(SOURCE_DIR, "ui_theme_metrics.h")
    ui_theme_checkbox_cpp = os.path.join(SOURCE_DIR, "ui_theme_checkbox.cpp")
    auto_profile_dialog_cpp = os.path.join(SOURCE_DIR, "auto_profile_dialog.cpp")
    config_profiles_machine_cpp = os.path.join(
        SOURCE_DIR, "config_profiles_machine.cpp")
    main_fan_runtime_cpp = os.path.join(SOURCE_DIR, "main_fan_runtime.cpp")
    main_gpu_front_cpp = os.path.join(SOURCE_DIR, "main_gpu_front.cpp")
    runtime_nvml_cpp = os.path.join(SOURCE_DIR, "main_runtime_nvml.cpp")
    gpu_backend_cpp = os.path.join(SOURCE_DIR, "gpu_backend.cpp")
    gpu_selection_config_cpp = os.path.join(SOURCE_DIR, "gpu_selection_config.cpp")
    main_runtime_control_cpp = os.path.join(SOURCE_DIR, "main_runtime_control.cpp")
    tray_autostart_cpp = os.path.join(SOURCE_DIR, "main_tray_autostart.cpp")
    startup_task_runtime_cpp = os.path.join(SOURCE_DIR, "main_startup_task_runtime.cpp")
    main_runtime_gpu_cpp = os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp")
    main_service_runtime_aggregate_cpp = os.path.join(SOURCE_DIR, "main_service_runtime.cpp")
    main_service_runtime_identity_cpp = os.path.join(SOURCE_DIR, "main_service_runtime_identity.cpp")
    main_service_fan_worker_cpp = os.path.join(SOURCE_DIR, "main_service_fan_worker.cpp")
    main_service_apply_runtime_cpp = os.path.join(SOURCE_DIR, "main_service_apply_runtime.cpp")
    main_service_persist_cpp = os.path.join(SOURCE_DIR, "main_service_persist.cpp")
    main_service_recovery_cpp = os.path.join(SOURCE_DIR, "main_service_recovery.cpp")
    main_service_recovery_clock_cpp = os.path.join(SOURCE_DIR, "main_service_recovery_clock.cpp")
    main_service_recovery_ledger_cpp = os.path.join(SOURCE_DIR, "main_service_recovery_ledger.cpp")
    main_service_controlled_restart_cpp = os.path.join(SOURCE_DIR, "main_service_controlled_restart.cpp")
    main_service_selected_gpu_pnp_cpp = os.path.join(SOURCE_DIR, "main_service_selected_gpu_pnp.cpp")
    selected_gpu_pnp_policy_h = os.path.join(SOURCE_DIR, "selected_gpu_pnp_policy.h")
    sessions_cpp = os.path.join(SOURCE_DIR, "main_service_sessions.cpp")
    lifecycle_events_cpp = os.path.join(SOURCE_DIR, "main_service_lifecycle_events.cpp")
    lifecycle_dxgi_cpp = os.path.join(SOURCE_DIR, "main_service_dxgi_readiness.cpp")
    lifecycle_apply_cpp = os.path.join(SOURCE_DIR, "main_service_lifecycle_apply.cpp")
    lifecycle_worker_cpp = os.path.join(SOURCE_DIR, "main_service_logon_coordinator.cpp")
    main_service_install_cpp = os.path.join(SOURCE_DIR, "main_service_install.cpp")
    startup_task_definition_cpp = os.path.join(SOURCE_DIR, "main_startup_task_definition.cpp")
    # The scheduled-logon / app-start code is deliberately isolated from the
    # profile UI.  Keep the fallback only for older trees that predate the split.
    logon_startup_cpp = os.path.join(SOURCE_DIR, "main_startup_profiles.cpp")
    if not os.path.exists(logon_startup_cpp):
        logon_startup_cpp = config_profiles_ui_cpp
    _lifecycle_surface = os.path.join(BUILD_WORK_DIR, "_service_lifecycle_surface.cpp")
    os.makedirs(BUILD_WORK_DIR, exist_ok=True)
    with open(_lifecycle_surface, "w", encoding="utf-8", errors="ignore") as _lf:
        for _cpp in (lifecycle_events_cpp, lifecycle_dxgi_cpp, lifecycle_apply_cpp,
                     lifecycle_worker_cpp):
            with open(_cpp, "r", encoding="utf-8", errors="ignore") as _source:
                _lf.write(_source.read())
                _lf.write("\n")
    main_service_logon_coordinator_cpp = _lifecycle_surface
    logon_coordinator_cpp = _lifecycle_surface
    _service_server_surface = os.path.join(BUILD_WORK_DIR, "_service_server_surface.cpp")
    with open(_service_server_surface, "w", encoding="utf-8", errors="ignore") as _sf:
        for _cpp in (service_request_policy_cpp, service_pipe_cpp,
                     service_host_cpp):
            with open(_cpp, "r", encoding="utf-8", errors="ignore") as _source:
                _sf.write(_source.read())
                _sf.write("\n")
    service_server_cpp = _service_server_surface
    _service_runtime_surface = os.path.join(BUILD_WORK_DIR, "_service_runtime_surface.cpp")
    with open(_service_runtime_surface, "w", encoding="utf-8", errors="ignore") as _rf:
        for _cpp in (main_service_runtime_identity_cpp,
                     main_service_fan_worker_cpp,
                     main_service_apply_runtime_cpp,
                     main_service_runtime_aggregate_cpp):
            with open(_cpp, "r", encoding="utf-8", errors="ignore") as _source:
                _rf.write(_source.read())
                _rf.write("\n")
    main_service_runtime_cpp = _service_runtime_surface
    _service_ipc_surface = os.path.join(BUILD_WORK_DIR, "_service_ipc_surface.cpp")
    with open(_service_ipc_surface, "w", encoding="utf-8", errors="ignore") as _if:
        for _cpp in (service_connection_cpp, service_client_commands_cpp,
                     service_admin_client_cpp, service_machine_config_cpp):
            with open(_cpp, "r", encoding="utf-8", errors="ignore") as _source:
                _if.write(_source.read())
                _if.write("\n")
    service_ipc_cpp = _service_ipc_surface
    ui_main_cpp = os.path.join(SOURCE_DIR, "ui_main.cpp")
    ui_main_controls_cpp = os.path.join(SOURCE_DIR, "ui_main_controls.cpp")
    ui_lock_checkbox_cpp = os.path.join(SOURCE_DIR, "ui_lock_checkbox.cpp")
    ui_main_window_cpp = os.path.join(SOURCE_DIR, "ui_main_window.cpp")
    vf_backends_cpp = os.path.join(SOURCE_DIR, "vf_backends.cpp")
    gpu_core_h = os.path.join(SOURCE_DIR, "gpu_core.h")
    service_protocol_h = os.path.join(SOURCE_DIR, "service_protocol.h")
    # The platform-neutral data model (NVAPI/NVML types, VfBackendSpec,
    # DesiredSettings + IPC validator, ServiceRequest/Response, NvmlApi) lives
    # in the shared gpu_core/service_protocol headers so the Linux backend can
    # use it.  The source checks below assert invariants that may live in any
    # shared header, so point shared_h/app_shared_h at their concatenation.
    _shared_surface = os.path.join(BUILD_WORK_DIR, "_shared_header_surface.h")
    os.makedirs(BUILD_WORK_DIR, exist_ok=True)
    with open(_shared_surface, "w", encoding="utf-8", errors="ignore") as _sf:
        for _h in (os.path.join(SOURCE_DIR, "app_shared.h"), gpu_core_h,
                   service_protocol_h):
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
    require_text(shared_h, "SERVICE_PROTOCOL_VERSION = 9", "service protocol version includes typed apply origins and lifecycle metadata")
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
    require_text(diagnostics_cpp, "MiniDumpWithThreadInfo",
        "crash dumps retain actionable thread records")
    require_text(diagnostics_cpp, "MiniDumpWithUnloadedModules",
        "crash dumps retain unloaded-module history")
    require_order(main_cpp,
        "SetUnhandledExceptionFilter(green_curve_unhandled_exception_filter);",
        "service_try_dispatch_controlled_restart_helper(&helperExitCode)",
        "standalone recovery helper installs crash dumping before dispatch")
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
    require_text(gpu_backend_cpp,
        "bool aHasBdf = gpu_adapter_has_valid_pci_location(a);",
        "strong GPU identity distinguishes whether each adapter has a BDF")
    require_text(gpu_backend_cpp,
        "return a->pciDomain == b->pciDomain && a->pciBus == b->pciBus &&\n        a->pciDevice == b->pciDevice &&\n        a->pciFunction == b->pciFunction;",
        "strong GPU identity includes the complete PCI domain/bus/device/function")
    require_text(config_utils_cpp, "selected_identity_version=",
        "configured GPU serializer stores a versioned stable PCI identity")
    require_text(gpu_selection_config_cpp,
        'format_configured_gpu_selection_section("gpu"',
        "selected GPU persistence uses the shared versioned identity serializer")
    require_text(gpu_selection_config_cpp, "write_config_sections_atomic",
        "selected GPU persistence replaces its section atomically")
    require_text(lifecycle_apply_cpp, "resolve_configured_gpu_selection(",
        "logon GPU targeting resolves the persisted stable identity")
    require_text(lifecycle_apply_cpp, "legacy GPU ordinal is unsafe on a multi-adapter system",
        "legacy multi-GPU ordinals fail closed before automatic writes")
    require_text(main_service_persist_cpp, "ServiceRestartReapplySnapshot", "restart-reapply snapshot persists target GPU identity")
    require_text(main_service_persist_cpp, "targetGpu", "restart-reapply snapshot carries target GPU")
    require_text(main_service_persist_cpp, "Restart snapshot GPU identity is not present", "controlled restart restore skips when its owned target GPU cannot be matched")
    require_text(main_state_sync_cpp, "service_sid_string_from_token", "service user path cache resolves the caller SID")
    require_text(main_state_sync_cpp, "g_serviceUserPathsSid", "service user path cache keys by session id plus SID")
    require_text(os.path.join(SOURCE_DIR, "service_lifecycle_policy.h"), "authenticationId", "session debounce identity includes authentication LUID")
    require_text(os.path.join(SOURCE_DIR, "service_lifecycle_policy.h"), "service_lifecycle_identity_equal", "session debounce compares session, SID, and authentication LUID")
    require_text(selected_gpu_pnp_policy_h, "selected_gpu_pnp_resolve_match_count",
        "selected-GPU DEVINST ambiguity decision is pure and fixture-tested")
    require_text(main_service_selected_gpu_pnp_cpp,
        "CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE",
        "selected GPU registers an exact Configuration Manager device-instance filter")
    require_text(main_service_selected_gpu_pnp_cpp,
        "SetupDiGetClassDevsW(\n        &GUID_DEVCLASS_DISPLAY",
        "selected GPU maps only among present display DEVINSTs")
    require_text(main_service_selected_gpu_pnp_cpp,
        "service_lifecycle_post_selected_gpu_removal();",
        "exact selected-GPU removal callback coalesces into the lifecycle worker")
    require_text(main_service_selected_gpu_pnp_cpp,
        "service_lifecycle_post_selected_gpu_arrival();",
        "exact selected-GPU arrival callback coalesces into the lifecycle worker")
    forbid_text_in_operation(main_service_selected_gpu_pnp_cpp,
        "static DWORD CALLBACK service_selected_gpu_notification_callback",
        "CreateThread", "selected-GPU CM callback never creates a thread")
    forbid_text_in_operation(main_service_selected_gpu_pnp_cpp,
        "static DWORD CALLBACK service_selected_gpu_notification_callback",
        "hardware_initialize", "selected-GPU CM callback never probes hardware")
    forbid_text_in_operation(main_service_selected_gpu_pnp_cpp,
        "static DWORD CALLBACK service_selected_gpu_notification_callback",
        "service_apply", "selected-GPU CM callback never applies settings")
    forbid_text_in_operation(main_service_selected_gpu_pnp_cpp,
        "static DWORD CALLBACK service_selected_gpu_notification_callback",
        "debug_log", "selected-GPU CM callback performs no file-backed logging")
    require_text(service_server_cpp,
        "service_prepare_selected_gpu_notification_before_running();",
        "service startup prepares the exact selected GPU notification before RUNNING")
    require_text(main_service_selected_gpu_pnp_cpp,
        "if (target->pciDomain != 0)",
        "selected-GPU PnP recovery rejects unsupported non-zero PCI domains")
    require_text(main_service_selected_gpu_pnp_cpp,
        '"service startup read-only target"',
        "startup registration is read-only and best effort")

    # Startup is fail-closed: controlled continuation validation, the lifecycle
    # worker, pipe listener, and PnP subscriptions are all ready before clients
    # can observe RUNNING. The pipe independently rejects requests while this
    # gate is closed, so an explicit apply cannot race the controlled arm step.
    service_main_anchor = "static void WINAPI service_main(DWORD argc, LPWSTR* argv)"
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "service_prepare_controlled_recovery_startup(argc, argv);",
        "service_start_lifecycle_worker(",
        "controlled startup validation precedes lifecycle worker startup")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "service_start_lifecycle_worker(",
        "WaitForMultipleObjects(2, pipeReadyOrExited",
        "lifecycle worker is ready before pipe readiness is accepted")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "WaitForMultipleObjects(2, pipeReadyOrExited",
        "RegisterDeviceNotificationW(",
        "pipe listener is ready before device notifications and RUNNING")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "RegisterDeviceNotificationW(",
        "service_prepare_selected_gpu_notification_before_running();",
        "class-wide notification registration precedes exact selected-GPU registration")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "service_prepare_selected_gpu_notification_before_running();",
        "service_arm_validated_controlled_recovery();",
        "selected-GPU notification preparation precedes controlled restore arming")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "service_arm_validated_controlled_recovery();",
        "InterlockedExchange(&g_serviceClientRequestsReady, 1);",
        "controlled recovery is armed before client requests are enabled")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "InterlockedExchange(&g_serviceClientRequestsReady, 1);",
        "g_serviceStatus.dwCurrentState = SERVICE_RUNNING;",
        "client startup gate is opened only at the final RUNNING transition")
    require_order_in_operation(service_pipe_cpp,
        "static DWORD WINAPI service_pipe_server_thread_proc(void*)",
        "&g_serviceClientRequestsReady",
        "switch (request.command)",
        "pipe rejects all commands while lifecycle startup remains gated")

    for forbidden, label in (
        ("debug_log", "file-backed logging"),
        ("CreateThread", "per-event thread creation"),
        ("hardware_initialize", "hardware probing"),
        ("service_apply_desired_settings", "hardware application"),
        ("service_reset_all", "hardware reset"),
        ("get_config_", "configuration I/O"),
    ):
        forbid_text_in_operation(service_host_cpp,
            "static DWORD WINAPI service_control_handler_ex", forbidden,
            f"SCM control handler performs no {label}")
    for power_poster in ("static void service_lifecycle_post_suspend(DWORD powerEventType)",
                         "static void service_lifecycle_post_resume(DWORD powerEventType)"):
        forbid_text_in_operation(lifecycle_events_cpp, power_poster,
            "debug_log", "power callback posting remains file-I/O-free")
        forbid_text_in_operation(lifecycle_events_cpp, power_poster,
            "CreateThread", "power callback posting never allocates a worker")
        forbid_text_in_operation(lifecycle_events_cpp, power_poster,
            "hardware_initialize", "power callback posting never probes hardware")
    require_text(service_server_cpp,
        '"successful service apply target"',
        "successful explicit/automatic service apply refreshes the selected GPU registration")
    require_order_in_operation(service_server_cpp, "case SERVICE_CMD_APPLY:",
        '"service apply pre-write target"',
        "&hardwareRequest,",
        "service apply binds the exact selected GPU before its sole write")
    require_order_in_operation(service_pipe_cpp, "case SERVICE_CMD_APPLY:",
        "lock_service_runtime();",
        "service_explicit_supersede_automatic_work_locked(",
        "explicit Apply serializes lifecycle/helper supersession under the runtime lock")
    require_order_in_operation(service_pipe_cpp, "case SERVICE_CMD_APPLY:",
        "service_auto_restore_is_locked_out(&currentLockout)",
        "&hardwareRequest,",
        "automatic client Apply rechecks sticky lockout immediately before its write")
    forbid_text_in_operation(main_service_controlled_restart_cpp,
        "static int service_run_controlled_restart_helper",
        "service_resolve_active_user_paths_for_startup",
        "controlled restart helper does not enter full service user-path startup before its handshake")
    require_text_in_operation(main_service_controlled_restart_cpp,
        "static bool service_launch_controlled_restart_helper",
        "GetExitCodeProcess(process.hProcess, &helperExitCode)",
        "controlled restart parent preserves the helper exit stage in diagnostics")
    require_text_in_operation(service_pipe_cpp, "case SERVICE_CMD_APPLY:",
        "if (explicitUserApply && proofRecorded)",
        "only explicit successful Apply enters the durable lockout/history acknowledgement branch")
    require_order_in_operation(service_pipe_cpp, "case SERVICE_CMD_APPLY:",
        "if (explicitUserApply && proofRecorded)",
        "service_clear_auto_restore_lockout();",
        "explicit successful Apply gates sticky-lockout clearing")
    require_text_count(service_server_cpp,
        "service_clear_auto_restore_lockout();", 1,
        "sticky lockout has exactly one explicit-success clear call site")
    require_text_count(service_server_cpp,
        "service_clear_restart_history();", 1,
        "recovery history has exactly one explicit-success clear call site")
    require_text(main_service_logon_coordinator_cpp,
        '"successful lifecycle logon target"',
        "successful lifecycle logon refreshes the selected GPU registration")
    require_order_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_logon()",
        '"lifecycle logon pre-write target"',
        "service_apply_desired_settings(&applyRequest",
        "logon binds the exact selected GPU before its sole write")
    require_order_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_standby_restore()",
        '"standby restore pre-write target"',
        "service_apply_desired_settings(&desired",
        "standby binds the exact selected GPU before its sole write")
    require_order_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_standby_restore()",
        "service_capture_mature_oc_apply_proof(",
        "service_apply_desired_settings(&desired",
        "standby captures a mature proof before mandatory pre-write invalidation")
    require_order_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_standby_restore()",
        "service_apply_desired_settings(&desired",
        "service_restore_mature_oc_apply_proof(",
        "standby restores a mature proof only after successful hardware apply")
    forbid_text_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_logon()",
        "writeAttempted = true",
        "logon lifecycle never assumes a hardware write happened")
    forbid_text_in_operation(main_service_logon_coordinator_cpp,
        "static void service_lifecycle_attempt_standby_restore()",
        "writeAttempted = true",
        "standby lifecycle never assumes a hardware write happened")
    require_text(main_service_selected_gpu_pnp_cpp,
        "explicit/logon/standby writes remain enabled",
        "unsupported/ambiguous PnP identity degrades recovery without blocking writes")
    require_order_after(service_server_cpp, "// Ordinary or externally requested shutdown.",
        "service_stop_selected_gpu_notification_best_effort",
        "service_shutdown_logon_apply_coordinator",
        "selected-GPU callback is deactivated before the lifecycle worker stops")
    require_text(diagnostics_cpp, "crash_artifact_data_dir", "service crash artifacts route through a process-appropriate data directory")
    require_text(diagnostics_cpp, "resolve_service_machine_data_dir", "service crash artifacts use the machine service data directory")
    require_text(config_profile_repair_cpp, "savedOffsetMagnitude = savedOffset < 0 ? -(long long)savedOffset", "profile repair avoids abs(INT_MIN) overflow")
    require_text(service_ipc_cpp, "pl_append_quoted_arg_w", "elevated helper command lines use argv-compatible quoting")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "pl_append_quoted_arg_w", "GUI elevated helper command lines use argv-compatible quoting")
    require_text(build_script, "_verify_cached_tool_binary", "cached tool binaries are verified against trusted pinned digests")
    require_text(build_script, "LLVM_MINGW_CLANG_SHA256", "llvm-mingw executable digest is pinned")
    require_text(service_ipc_cpp, "wait_for_helper_process_bounded", "elevated helper waits are bounded")
    require_text(config_profiles_ui_cpp, "repair needed", "broken installed service advertises repair state")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "Repair and restart the background service", "broken installed service click repairs instead of removing")
    forbid_text(config_profiles_ui_cpp, "maybe_load_selected_profile_to_gui_without_apply", "startup cannot restore saved selected-slot intent as live GPU state")
    require_text(logon_startup_cpp, "show_live_gpu_state_for_disabled_app_launch", "disabled app-start refreshes the editor from the service snapshot")
    require_text(logon_startup_cpp, "saved slot deliberately not loaded", "startup diagnostics distinguish saved profile intent from live state")
    require_order_in_operation(logon_startup_cpp,
        "static void show_live_gpu_state_for_disabled_app_launch()",
        "refresh_service_snapshot_and_active_desired(",
        "update_all_gui_for_service_state();",
        "startup live-state refresh repaints controls only after receiving the service snapshot")
    require_text(desired_settings_helpers_cpp, "desired_settings_match_active_service_intent", "profile intent can be compared to the service active desired state")
    require_text(config_profiles_ui_cpp, "sync_applied_profile_from_service_metadata", "applied profile indicator follows service ownership metadata")
    require_text(config_profiles_ui_cpp, "Never infer it by comparing", "expected VF drift never invalidates profile ownership")
    forbid_text(config_profiles_ui_cpp, "profile_mismatches_live_hardware", "absolute live VF MHz must not decide profile ownership")
    require_text(logon_startup_cpp, "already active in background service; skipping reset-before-apply", "app-start auto-load skips disruptive reset/apply when service already owns the same intent")
    require_order(logon_startup_cpp,
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
    require_text(ui_main_controls_cpp, "gpuSelectY = dp(10)", "GPU selector lives in the graph header gap")
    require_text(main_layout_policy_h, "MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL", "main graph has a tested readable minimum")
    require_text(main_layout_policy_h, "horizontalOverflow", "layout policy exposes lossless horizontal overflow")
    require_text(main_layout_policy_h, "verticalOverflow", "layout policy exposes lossless vertical overflow")
    require_text(ui_main_layout_cpp, "A scrollbar changes the perpendicular client dimension", "main layout converges both scrollbar dimensions")
    require_text(ui_main_layout_cpp, "APP_WM_ENSURE_LAYOUT_FOCUS", "keyboard focus scrolls off-screen controls into view")
    require_text(ui_main_layout_cpp, "main window clamped to work area", "main window cannot remain outside monitor work area")
    require_text(ui_main_layout_cpp, "main content growth:", "populated VF content growth is diagnosed")
    require_text(entry_cpp, "main_window_initial_rect", "main window starts from centered or restored placement policy")
    forbid_text(entry_cpp, "CW_USEDEFAULT",
        "main window creation never delegates placement to the top-left-biased OS default")
    require_text(ui_main_layout_cpp, '"ui", "main_window_placement_version"',
        "main window placement uses a committed per-user version marker")
    require_text(ui_main_layout_cpp, "main_layout_resize_around_center",
        "late VF-content growth preserves the window center")
    require_text(ui_main_window_cpp, "persist_main_window_placement(hwnd)",
        "main window normal placement is persisted on close")
    require_text(ui_theme_metrics_h, "ui_theme_checkbox_box_size",
        "ordinary themed checkboxes share one DPI-aware box metric")
    require_text(ui_theme_checkbox_cpp, "ui_theme_checkbox_box_size",
        "shared checkbox renderer uses the canonical box metric")
    require_text(auto_profile_dialog_cpp, "WM_CTLCOLORLISTBOX",
        "auto-profile dropdown lists use the dark dialog palette")
    require_text(auto_profile_dialog_cpp, "draw_themed_checkbox_control",
        "auto-profile checkboxes use the main-window themed renderer")
    require_text(auto_profile_dialog_cpp, "BS_OWNERDRAW",
        "auto-profile checkboxes are owner-drawn")
    require_text(auto_profile_dialog_cpp, "UiCheckboxState",
        "auto-profile owner-draw checkboxes keep explicit dialog-model state")
    require_text(auto_profile_dialog_cpp, "RDW_INVALIDATE | RDW_UPDATENOW",
        "auto-profile checkbox clicks synchronously repaint their new state")
    forbid_text(auto_profile_dialog_cpp, "BM_GETCHECK",
        "owner-draw auto-profile checkboxes do not query unsupported native check state")
    forbid_text(auto_profile_dialog_cpp, "BM_SETCHECK",
        "owner-draw auto-profile checkboxes do not write unsupported native check state")
    forbid_text(auto_profile_dialog_cpp, "BS_AUTOCHECKBOX",
        "auto-profile dialog no longer uses unmatched native checkboxes")
    require_text(ui_main_layout_cpp, "SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE", "scrolling moves children and erases exposed pixels atomically")
    require_order_in_operation(ui_main_layout_cpp,
        "static bool main_layout_set_scroll(HWND hwnd, int x, int y)",
        "ScrollWindowEx(hwnd, oldX - x, oldY - y",
        "RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW",
        "scrolling synchronously redraws the complete settled frame")
    require_order_in_operation(os.path.join(SOURCE_DIR, "ui_main_control_lifecycle.cpp"),
        "static void rebuild_edit_controls()",
        "main_layout_grow_window_for_content(",
        "SendMessageA(hwnd, WM_SETREDRAW, FALSE",
        "window grows toward populated VF content before controls are rebuilt")
    require_text(ui_main_window_cpp, "case WM_DPICHANGED:", "main window relayouts on per-monitor DPI changes")
    require_text(ui_main_window_cpp, "case WM_DISPLAYCHANGE:", "main window revalidates work area on display changes")
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
    # GUI lock-checkbox regressions:
    #  (1) the FLATTEN tick must reuse the anti-aliased renderer shared by the themed
    #      checkboxes (service install / share-all-users / tray) instead of a jagged
    #      raw-GDI Polyline, so it does not look corrupted next to them.
    #  (2) only BN_CLICKED may advance the tri-state. BS_OWNERDRAW emits BN_DBLCLK
    #      automatically, while focus codes require BS_NOTIFY (which these controls
    #      do not use). Every notification is logged before the policy filters it.
    #  (3) an armed gesture is consumed once and rejected if the lock model changes
    #      between press and release (for example, a startup service snapshot).
    require_text(main_shell_cpp, "draw_checkbox_tick_smooth(hdc, &box, RGB(0xE8, 0xF2, 0xFF))", "lock FLATTEN tick uses the shared anti-aliased checkmark renderer")
    forbid_text(main_shell_cpp, "Polyline(hdc, pts, 3)", "lock checkbox no longer draws the jagged raw-GDI checkmark")
    require_text(ui_main_window_cpp, "decide_lock_activation(", "lock tri-state commands use the executable activation policy")
    require_text(ui_main_window_cpp, "lock checkbox command: vi=%d notify=%u decision=", "all lock notifications and decisions are logged")
    require_text(ui_lock_checkbox_cpp, "activate_lock_checkbox_once", "lock tri-state transition is centralized")
    require_text(ui_lock_checkbox_cpp, "lock_checkbox_subclass_proc", "lock checkbox subclass exists")
    require_text(ui_lock_checkbox_cpp, "WM_LBUTTONDBLCLK", "lock checkbox double-click cannot advance the tri-state twice")
    require_text(ui_lock_checkbox_cpp, "paired double-click release suppressed", "double-click paired release cannot become an unarmed click")
    require_text(ui_main_controls_cpp, "lock checkbox subclass install FAILED", "subclass installation failure is diagnosed")
    require_text(ui_main_controls_cpp, "SetLastError(ERROR_SUCCESS);", "subclass failure logging does not report a stale Win32 error")
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
    require_text(build_script, '"-gcodeview"',
        "Windows builds retain CodeView records for actionable crash dumps")
    require_text(build_script, '"-Wl,--pdb=',
        "Windows x64 links emit matching private PDB symbols")
    require_text(build_script, '"--only-keep-debug"',
        "Windows ARM64 builds retain a matching private DWARF debug artifact")
    require_text(build_script, 'DIST_DIR, "symbols"',
        "private PDBs stay outside release payload directories")
    require_text(build_script, "verify_windows_private_symbols",
        "Windows builds structurally verify every private symbol artifact")
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
    service_runtime_cpp = main_service_runtime_cpp
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
    require_text(main_service_runtime_cpp, "thread handle preserved", "fan thread handle is preserved on timeout to prevent replacement")

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
    require_text(service_server_cpp, "(A;;GRGW;;;AU)", "pipe ACL admits authenticated local users read+write")
    forbid_text(service_server_cpp, "GRGW;;;%s", "pipe ACL must not bake a per-user console SID (stale-ACL lockout)")
    require_text(service_server_cpp, "cannot create restricted ACL, failing listener closed", "pipe creation fails closed when the SD cannot be built")
    # F-15-003: Service listens for Windows session logon events so it can apply
    # a machine-wide default profile for users who have no per-user logon slot.
    require_text(service_server_cpp, "SERVICE_ACCEPT_SESSIONCHANGE", "service accepts session-change notifications")
    require_text(sessions_cpp, "WTS_SESSION_LOGON", "service handles WTS_SESSION_LOGON")
    require_text(sessions_cpp, "service_load_logon_profile_from_context", "service resolves an immutable per-session user/shared profile on logon")
    require_order_in_operation(sessions_cpp,
        "static ServiceLogonProfileResolveResult service_load_logon_profile_from_context(",
        "case LOGON_PROFILE_SOURCE_MACHINE_DEFAULT:",
        "bind_machine_gpu(machineSlot)",
        "machine default logon applies use the GPU binding published with the shared slot")
    require_order_in_operation(sessions_cpp,
        "static ServiceLogonProfileResolveResult service_load_logon_profile_from_context(",
        "LogonProfileSource selected = resolve_logon_profile_source(",
        "bind_user_gpu()",
        "per-user GPU config is required only after the resolver chooses a per-user profile")
    require_text(service_request_policy_cpp,
        "service_apply_shared_only_policy", "restricted manual applies use one authoritative service policy helper")
    require_text_in_operation(service_request_policy_cpp,
        "static bool service_apply_shared_only_policy(",
        "ServicePolicyConfigLockGuard policyLock",
        "restricted policy/settings/GPU reads share one config transaction")
    require_text(service_request_policy_cpp,
        "service_resolve_configured_gpu_target", "restricted shared applies resolve the published stable GPU identity")
    require_text(service_request_policy_cpp,
        "single-adapter compatibility check required", "legacy shared slots are explicitly limited to safe single-GPU compatibility")
    # Active-user session router: only a real logon authorizes apply; logoff
    # cancels matching state and connect/disconnect/unlock are readiness cues
    # (FUS-safe, identity-bound). A plain service start while a user is already
    # logged in must stay non-mutating; installing/repairing the service should
    # not silently apply that user's logon slot.
    require_text(sessions_cpp, "service_handle_session_change", "active-user session-change router exists")
    require_text(logon_coordinator_cpp, "service_lifecycle_post_session_event", "session callbacks coalesce events into the lifecycle worker")
    # Logon authorization is event-only.  Fast Startup/autologon use the real
    # authenticated task handoff, coalesced with WTS; service startup must never
    # infer a login or use the removed boot markers.
    require_text(shared_h, "SERVICE_CMD_LOGON_HANDOFF", "protocol has a settings-free authenticated logon handoff")
    require_text(entry_cpp, "SERVICE_CMD_LOGON_HANDOFF", "every --logon-start invocation notifies the service")
    require_text(service_server_cpp, "SERVICE_CMD_LOGON_HANDOFF", "service accepts the authenticated logon handoff")
    forbid_text(sessions_cpp, "service_maybe_reconcile_active_session_at_boot", "service startup must not synthesize a logon event")
    forbid_text(shared_h, "should_reconcile_active_session_at_boot", "boot-reconcile inference policy must stay removed")
    require_text(main_service_persist_cpp, "service_cleanup_obsolete_recovery_artifacts", "obsolete boot/recovery artifacts are deleted during migration")
    forbid_text(main_service_persist_cpp, "service_mark_boot_reconcile_done", "obsolete boot-reconcile authorization API must stay removed")
    forbid_text(main_service_persist_cpp, "service_mark_first_service_start_this_boot", "obsolete first-start authorization API must stay removed")
    require_text(service_server_cpp, "service_lifecycle_post_session_event", "SESSIONCHANGE handler routes through the lifecycle worker")
    require_text(service_server_cpp, "g_servicePipeRecycleEvent", "pipe ACL is recycled on active-user change")
    # Per-user logon task is registered for the REQUESTING user, not the
    # approving admin, when the elevated helper runs on their behalf.
    require_text(service_ipc_cpp, "--for-user", "elevated startup-task helper forwards the requesting user")
    require_text(startup_task_runtime_cpp, "set_forced_startup_user_sam", "startup task can be scoped to the requesting user")
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
    require_text(config_profiles_machine_cpp, "copy_profile_slot_to_machine_config", "shared bank copy helper exists")
    require_text(config_profiles_machine_cpp, "clear_machine_profile_slot", "shared bank clear helper exists")
    require_text(config_profiles_machine_cpp, '"profile%d_gpu"',
        "published shared slots carry a per-slot GPU binding")
    require_text(config_profiles_machine_cpp, "replace_machine_profile_slot_sections",
        "profile settings and GPU binding fail closed as one slot")
    require_text(config_profiles_machine_cpp, 'state=publishing',
        "interrupted shared-slot publication remains durably unavailable")
    require_text(config_profiles_machine_cpp, 'state=committed',
        "shared-slot publication becomes visible only after a final commit marker")
    require_text(config_profiles_machine_cpp,
        "a present but\n        // malformed binding is never downgraded",
        "legacy compatibility cannot hide a malformed GPU binding")
    require_text(config_profiles_machine_cpp, 'replaced[replaceCount++] = "controls"',
        "slot-1 fail-closed cleanup also removes legacy profile aliases")
    require_text(config_profiles_machine_cpp, "published GPU identity readback mismatch",
        "published GPU bindings receive locked identity readback verification")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--publish-slot-to-machine", "CLI supports publishing a slot to the shared bank")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--clear-machine-slot", "CLI supports clearing a shared bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_PUBLISH_ID", "GUI advanced menu can publish a bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID", "GUI advanced menu can clear a bank slot")
    # F-15-008: One coherent "share with all users" action couples publishing the
    # slot data with setting it as the all-users default (the old footgun was
    # setting a default that resolved to an empty bank slot).
    require_text(config_profiles_machine_cpp, "share_profile_slot_for_all_users", "coherent share helper publishes data AND sets the default")
    require_text(config_profiles_machine_cpp, "unshare_profile_slot_for_all_users", "coherent unshare helper clears data AND the default")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--share-slot", "CLI supports the coherent share action")
    require_text(os.path.join(SOURCE_DIR, "entry.cpp"), "--unshare-slot", "CLI supports the coherent unshare action")
    require_text(ui_main_window_cpp, "SHARE_ALL_USERS_CHECK_ID", "GUI has the share-with-all-users checkbox handler")
    # F-15-009: Any user can load the admin-published shared profiles on demand
    # (read-only) and apply them via the service, not just at logon.
    require_text(ui_main_window_cpp, "show_shared_profiles_menu", "GUI surfaces shared profiles for on-demand load")

    # F-15-010: Deleting a logon task that requires elevation (admin-created /
    # HighestAvailable) must fall back to the elevated helper instead of trusting
    # the schtasks exit code and reporting a dead-end "still exists" error.
    require_text(startup_task_runtime_cpp, "outNeedsElevation", "direct startup-task path signals when elevation is required")
    require_text(startup_task_runtime_cpp, "needs elevation", "startup-task delete that leaves the task present requests elevation")
    require_text(startup_task_runtime_cpp, "schtasks /delete exit=", "schtasks delete logs its exit code for diagnosis")
    require_text(startup_task_runtime_cpp, "schtasks /create exit=", "schtasks create logs its exit code for diagnosis")
    forbid_text(startup_task_runtime_cpp, "exitCode != 0 && exitCode != 1", "startup-task delete must verify by state, not accept schtasks exit 1 as success")
    forbid_text(startup_task_runtime_cpp, "Startup task still exists after delete", "dead-end delete error replaced by an elevation-aware fallback")

    # Startup-readiness regression (reported July 2026): a task can run during
    # the gap where SCM says the service is running but its pipe/GPU snapshot is
    # not usable.  The service is the *only* automatic logon hardware writer;
    # the independent --tray-start GUI may observe/reconnect, but must never turn
    # that readiness transition into a second APPLY or reset.
    require_text(logon_startup_cpp, "static void apply_logon_startup_behavior",
                 "logon tray GUI has an explicit service-observer path")
    require_text(logon_startup_cpp, "start_service_reconnect_timer_if_needed",
                 "logon tray GUI uses the existing service reconnect observer")
    require_text(logon_startup_cpp, "refresh_background_service_state",
                 "logon tray GUI observes current service state")
    forbid_text_in_operation(logon_startup_cpp,
                             "static void apply_logon_startup_behavior",
                             "apply_desired_settings(",
                             "logon tray GUI must not perform a hardware apply")
    forbid_text_in_operation(logon_startup_cpp,
                             "static void apply_logon_startup_behavior",
                             "resetOcBeforeApply",
                             "logon tray GUI must not request a reset-before-apply")
    forbid_text_in_operation(logon_startup_cpp,
                             "static void apply_logon_startup_behavior",
                             "load_profile_from_config(",
                             "logon tray GUI leaves profile resolution to the service")
    forbid_text(logon_startup_cpp, "apply_logon_shared_slot_if_configured",
                "obsolete GUI shared-logon apply helper is removed with service ownership")
    forbid_text(logon_startup_cpp, "logon_wait_for_gpu_driver_ready",
                "old one-shot GUI logon wait cannot permanently skip a profile")
    require_text(main_gpu_front_cpp, "start_service_reconnect_timer_if_needed",
                 "shared service reconnect observer remains available to the logon GUI path")
    require_text_in_operation(ui_main_window_cpp,
                              "if (wParam == SERVICE_RECONNECT_TIMER_ID)",
                              "refresh_background_service_state()",
                              "reconnect timer observes service readiness")
    require_text_in_operation(ui_main_window_cpp,
                              "if (wParam == SERVICE_RECONNECT_TIMER_ID)",
                              "refresh_curve()",
                              "reconnect timer refreshes GUI state after service readiness")
    forbid_text_in_operation(ui_main_window_cpp,
                             "if (wParam == SERVICE_RECONNECT_TIMER_ID)",
                             "apply_desired_settings(",
                             "reconnect timer must not issue a hardware apply")
    forbid_text_in_operation(ui_main_window_cpp,
                             "if (wParam == SERVICE_RECONNECT_TIMER_ID)",
                             "resetOcBeforeApply",
                             "reconnect timer must not request a reset-before-apply")
    require_text_in_operation(entry_cpp,
                              "if (opts.logonStart)",
                              "service_client_logon_handoff",
                              "scheduled logon sends the authenticated service handoff")
    require_text_in_operation(entry_cpp,
                              "if (opts.logonStart)",
                              "g_cliExitCode = 1;",
                              "failed scheduled handoff exits nonzero for Task Scheduler diagnostics")
    forbid_text_in_operation(entry_cpp,
                             "if (opts.logonStart)",
                             "g_app.startHiddenToTray = true",
                             "bounded --logon-start task must never become the resident tray process")
    require_text_in_operation(entry_cpp,
                              "if (opts.trayStart)",
                              "g_app.startHiddenToTray = true",
                              "separate --tray-start invocation owns resident tray startup")
    require_text_in_operation(entry_cpp,
                              "if (opts.logonStart)",
                              "return true;",
                              "silent scheduled logon exits after handing ownership to the service")
    forbid_text_in_operation(entry_cpp,
                             "if (opts.logonStart)",
                             "opts.applyConfig = true;",
                             "silent --logon-start must not enter the CLI profile-apply path")
    forbid_text_in_operation(entry_cpp,
                             "if (opts.logonStart)",
                             "opts.logonStart = false;",
                             "silent --logon-start remains an explicit service handoff")
    forbid_text(entry_cpp, "deferInitialLogonServiceCheck",
                "obsolete CLI logon readiness-and-apply workaround is removed")
    forbid_text(entry_cpp, "CLI logon shared apply",
                "obsolete CLI shared-logon hardware apply is removed")
    require_order(entry_cpp,
                  "if (handle_cli(wCmdLine))",
                  "if (!acquire_single_instance_mutex())",
                  "one-shot logon handoff runs before resident-GUI single-instance handling")
    require_text_in_operation(service_client_commands_cpp,
        "static bool wait_for_background_service_running_notification(",
        "NotifyServiceStatusChangeW(",
        "scheduled handoff waits for SCM readiness through status notification")
    require_text_in_operation(service_client_commands_cpp,
        "static bool wait_for_background_service_running_notification(",
        "SERVICE_NOTIFY_START_PENDING",
        "SCM readiness handles the initial STOPPED to START_PENDING transition")
    require_text_in_operation(service_client_commands_cpp,
        "static bool wait_for_background_service_running_notification(",
        "SERVICE_NOTIFY_RUNNING",
        "SCM readiness subscribes to the RUNNING transition")
    require_text_in_operation(service_client_commands_cpp,
        "static bool wait_for_background_service_running_notification(",
        "SleepEx(remaining, TRUE)",
        "SCM readiness uses an alertable notification wait")
    forbid_text_in_operation(service_client_commands_cpp,
        "static bool wait_for_background_service_running_notification(",
        "Sleep(",
        "scheduled handoff readiness never polls with a blind sleep")
    require_order_in_operation(service_client_commands_cpp,
        "static bool service_client_logon_handoff(",
        "wait_for_background_service_running_notification(120000",
        "service_send_request(&request",
        "handoff waits up to 120 seconds for RUNNING before its sole IPC attempt")

    require_text_in_operation(logon_startup_cpp,
        "static void maybe_load_app_launch_profile_to_gui()",
        "STARTUP_EDITOR_SOURCE_LOGON_SERVICE",
        "tray logon startup suppresses independent app-launch profile automation")

    # Pin every producer to its typed origin. Enum unit tests alone would not
    # catch an app-launch/foreground caller accidentally claiming an explicit
    # origin and clearing the sticky lockout after success.
    require_text(logon_startup_cpp, "SERVICE_APPLY_ORIGIN_APP_LAUNCH",
                 "app-launch automation uses its automatic origin")
    require_text(os.path.join(SOURCE_DIR, "auto_profile_win32.cpp"),
                 "SERVICE_APPLY_ORIGIN_FOREGROUND",
                 "foreground automation uses its automatic origin")
    require_text(os.path.join(SOURCE_DIR, "auto_profile_win32.cpp"),
                 "SERVICE_APPLY_ORIGIN_HOTKEY",
                 "profile hotkeys use their explicit origin")
    require_text(os.path.join(SOURCE_DIR, "auto_profile_win32.cpp"),
                 "SERVICE_APPLY_ORIGIN_TRAY",
                 "tray profile selections use their explicit origin")
    require_text(ui_main_window_cpp, "SERVICE_APPLY_ORIGIN_GUI",
                 "GUI Apply uses its explicit origin")
    require_text(entry_cpp, "SERVICE_APPLY_ORIGIN_CLI",
                 "explicit CLI Apply uses its explicit origin")
    require_text(service_request_policy_cpp,
                 "service_apply_origin_is_client_apply(origin)",
                 "service APPLY accepts only the client-origin whitelist")

    # The scheduled task itself is app-owned configuration, not merely an
    # existence flag.  Definitions are classified: delayed/elevated definitions
    # with the right identity/action remain compatible and are normalized only
    # best-effort; disabled/wrong-user/wrong-action definitions are broken.  This
    # deliberately does not introduce a Task Scheduler retry.
    require_text(main_shell_cpp, '#include "main_startup_task_runtime.cpp"',
                 "startup-task runtime shard is compiled into the Windows shell")
    require_text(main_shell_cpp, '#include "main_tray_autostart.cpp"',
                 "independent tray-autostart shard is compiled into the Windows shell")
    require_text(tray_autostart_cpp, "HKEY_CURRENT_USER",
                 "resident tray startup uses a per-user Windows Run entry")
    require_text(tray_autostart_cpp, "--tray-start --config",
                 "resident tray launch uses a distinct internal argument")
    forbid_text_in_operation(main_fan_runtime_cpp,
                             "static bool should_enable_startup_task_from_config",
                             "is_start_on_logon_enabled",
                             "tray residency must not keep the bounded handoff task enabled")
    require_text_in_operation(main_fan_runtime_cpp,
                              "static ConfigEnablementState startup_task_config_state",
                              "resolve_machine_config_path",
                              "an effective all-users profile keeps authenticated task redundancy")
    require_text(main_shell_cpp, '#include "main_startup_task_definition.cpp"',
                 "startup-task XML validator is compiled into the Windows shell")
    require_text(startup_task_definition_cpp, "startup_task_query_xml",
                 "existing startup task XML is queried before it is accepted")
    require_text(startup_task_definition_cpp, "/query /tn",
                 "startup-task verifier invokes schtasks XML query")
    require_text(startup_task_definition_cpp, "missing LogonTrigger",
                 "startup-task verifier requires a user logon trigger")
    require_text(startup_task_definition_cpp, "startup_task_definition_classify_xml",
                 "startup-task XML classification is pure and fixture-testable")
    require_text(startup_task_definition_cpp, "compatible legacy logon delay",
                 "startup-task classifier keeps delayed legacy handoffs functional")
    require_text(startup_task_definition_cpp, "compatible legacy HighestAvailable principal",
                 "startup-task classifier keeps elevated legacy handoffs functional")
    require_text(startup_task_definition_cpp, "logon trigger is disabled",
                 "startup-task classifier rejects explicit trigger disablement")
    require_text(startup_task_definition_cpp, "task is disabled",
                 "startup-task classifier rejects explicit task disablement")
    require_text(startup_task_definition_cpp, "action command differs",
                 "startup-task classifier rejects stale executable actions")
    require_text(startup_task_definition_cpp, "CommandLineToArgvW(actual, &argc)",
                 "startup-task verifier compares parsed logon command arguments")
    require_text(startup_task_definition_cpp, "MultipleInstancesPolicy",
                 "startup-task verifier pins its single-instance policy")
    require_text(startup_task_definition_cpp, "extra or duplicate task trigger",
                 "startup-task verifier rejects additional triggers")
    require_text(startup_task_definition_cpp, "battery power can prevent task start",
                 "startup-task verifier rejects battery gating")
    require_text(startup_task_definition_cpp, "RestartOnFailure",
                 "startup-task verifier rejects scheduler repetition")
    require_text(startup_task_runtime_cpp, "STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY",
                 "startup-task sync distinguishes compatible legacy definitions")
    require_text(startup_task_runtime_cpp, "preserving functional legacy definition",
                 "failed best-effort normalization keeps a functional legacy task")
    require_text(startup_task_runtime_cpp, "broken or unreadable",
                 "startup-task sync repairs broken definitions")
    require_text(startup_task_runtime_cpp, "created/repaired and verified",
                 "startup-task sync verifies the replacement task after creation")
    require_text(startup_task_runtime_cpp,
                 "synchronize_startup_task_preserving_indeterminate(",
                 "enabled existing startup tasks are periodically validated and repaired")
    forbid_text(startup_task_runtime_cpp, "PT15S",
                "generated startup task has no fixed Task Scheduler logon delay")
    require_text(startup_task_runtime_cpp, 'L"PT3M"',
                 "generated startup task has the canonical three-minute execution limit")

    # The service owns the authoritative logon path too.  Retry only identity,
    # profile-materialization, and driver-readiness prerequisites; an actual GPU
    # apply failure must finish the generation without replaying hardware writes.
    require_text(main_service_runtime_cpp, "enum ServiceLogonProfileResolveResult",
                 "service distinguishes logon profile readiness from GPU apply failure")
    require_text(main_service_runtime_cpp, "SERVICE_LOGON_PROFILE_TRANSIENT",
                 "service treats a still-materializing configured profile as transient")
    require_text(sessions_cpp, "eligiblePerUserPending || sharedPending",
                 "service does not mistake configured-but-not-yet-readable profiles for no profile")
    require_text(os.path.join(SOURCE_DIR, "service_lifecycle_policy.h"), "struct ServiceLifecycleState",
                 "service has a pure coalesced lifecycle state")
    require_text(logon_coordinator_cpp, "service_lifecycle_thread_proc",
                 "service uses one long-lived logon/lifecycle worker")
    require_text(lifecycle_events_cpp, "FindFirstChangeNotificationA(",
                 "transient profile materialization arms a real config-directory readiness watch")
    require_text(lifecycle_events_cpp, "FindNextChangeNotification(",
                 "lifecycle worker rearms config readiness notifications after each change")
    require_text(lifecycle_events_cpp, "service_lifecycle_config_file_stamp(",
                 "broad directory notifications are filtered by exact config-file identity and metadata")
    require_text(lifecycle_events_cpp, "bool ancestorProgress =",
                 "config readiness moves an ancestor watch inward before exact config creation")
    require_text(lifecycle_events_cpp, "service_lifecycle_update_config_watch(targetPath",
                 "failed/repositioned config watches are immediately re-established")
    require_text(lifecycle_worker_cpp,
                 "bool shouldAttemptLifecycle = lifecycleWake || configReadinessSignal ||",
                 "sibling-file directory activity cannot spin the pending lifecycle resolver")
    require_text(lifecycle_dxgi_cpp, "RegisterAdaptersChangedEvent",
                 "DXGI adapter-set changes provide user-mode driver readiness")
    require_text(lifecycle_worker_cpp, "dxgiAdapterReadinessSignal",
                 "lifecycle worker waits on the DXGI adapter readiness event")
    forbid_text(lifecycle_dxgi_cpp, "Sleep(",
                "DXGI readiness uses no timing sleep")
    forbid_text(lifecycle_dxgi_cpp, "SetTimer(",
                "DXGI readiness uses no timer retry")
    forbid_text(lifecycle_dxgi_cpp, "CreateThread(",
                "DXGI readiness reuses the one long-lived lifecycle worker")
    forbid_text(lifecycle_dxgi_cpp, "hardware_initialize(",
                "DXGI event registration does not poll or touch GPU hardware")
    require_text(lifecycle_worker_cpp,
                 "WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE)",
                 "GPU readiness remains observable for the service lifetime without a deadline")
    require_text(main_service_runtime_identity_cpp,
                 "WTSEnumerateSessions failed",
                 "active-session enumeration failure remains a transient fail-closed prerequisite")
    require_text(os.path.join(SOURCE_DIR, "main_service_fan_worker.cpp"),
                 "if (!g_app.gpuHandle || !g_app.loaded)",
                 "visible-GUI telemetry uses the service cache instead of repeating full hardware initialization")
    forbid_text(os.path.join(SOURCE_DIR, "main_service_pipe.cpp"),
                '"serialized telemetry probe confirmed GPU readiness"',
                "routine telemetry cannot wake lifecycle restoration work")
    require_text(os.path.join(SOURCE_DIR, "main_service_connection.cpp"),
                 "g_verifiedServicePipeCreationTime",
                 "repeated telemetry authenticates the already-verified service process generation without repeated SCM/file queries")
    require_text(os.path.join(SOURCE_DIR, "main_service_connection.cpp"),
                 "CompareFileTime(&pipeProcessCreation",
                 "service identity cache is bound to process creation time rather than reusable PID alone")
    require_text(os.path.join(SOURCE_DIR, "config_profiles_ui.cpp"),
                 "applied_profile_sync_inputs_unchanged(inputs)",
                 "routine telemetry cannot repeatedly reload an unchanged saved profile from INI")
    require_text(os.path.join(SOURCE_DIR, "config_profile_sync_cache.cpp"),
                 "GetFileAttributesExA(path, GetFileExInfoStandard",
                 "profile ownership cache invalidates on an external config-file change without reading its contents")
    require_text(lifecycle_events_cpp, "logonSessionEventPending",
                 "real WTS logon is preserved separately from generic session readiness cues")
    require_order_in_operation(lifecycle_worker_cpp,
        "static DWORD WINAPI service_lifecycle_thread_proc(void*)",
        "if (inbox.logonSessionEventPending",
        "if (inbox.sessionEventPending)",
        "preserved WTS logon is drained even when a later generic session event overwrote the inbox")
    require_order_in_operation(sessions_cpp,
        "static void service_handle_session_change(DWORD eventType, DWORD eventSessionId)",
        "if (eventType != WTS_SESSION_LOGON) return;",
        "service_lifecycle_worker_queue_logon(",
        "connect/disconnect/unlock can signal readiness but only WTS logon authorizes a profile")
    require_text(shared_h, "SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY",
                 "service coordinator has an explicit retryable pre-apply outcome")
    require_text(shared_h, "SERVICE_LIFECYCLE_RESULT_FAILED",
                 "service coordinator distinguishes an actual apply failure")
    forbid_text(sessions_cpp, "SERVICE_SESSION_LOGON_READY_TIMEOUT_MS",
                "logon prerequisite intent must not expire on an arbitrary deadline")
    require_text(logon_coordinator_cpp, "WaitForMultipleObjects",
                 "service readiness retry is interruptible by stop/session events")
    forbid_text(logon_coordinator_cpp, "Sleep(",
                "service logon coordinator uses readiness signals instead of blind sleeps")
    require_text(logon_coordinator_cpp, "unresolvedLogonPending",
                 "session router queues a logon even when the SID/token is temporarily unavailable")
    require_text(os.path.join(SOURCE_DIR, "main_state_sync.cpp"), "TokenStatistics",
                 "session identity includes the authentication LUID")
    require_text(os.path.join(SOURCE_DIR, "main_state_sync.cpp"), "AuthenticationId",
                 "session identity stores TokenStatistics.AuthenticationId")
    require_text(sessions_cpp, "WTS_SESSION_LOGOFF",
                 "logoff cancels matching pending/debounce state")
    require_text(logon_coordinator_cpp, "Once a hardware write is",
                 "GPU apply failures are explicitly terminal for automatic logon handling")
    require_order_in_operation(logon_coordinator_cpp,
                        "static void service_lifecycle_attempt_logon()",
                        "logoffGeneration))",
                        "&applyRequest,",
                        "coordinator commits its final generation check before GPU mutation")
    require_order_in_operation(logon_coordinator_cpp,
                        "static void service_lifecycle_attempt_logon()",
                        "lock_service_runtime();",
                        "identityCheck = service_verify_active_session_identity(",
                        "coordinator serializes the final identity probe under the runtime lock")
    require_order_in_operation(logon_coordinator_cpp,
                        "static void service_lifecycle_attempt_logon()",
                        "identityCheck = service_verify_active_session_identity(",
                        "service_lifecycle_authorize_logon_write",
                        "coordinator rechecks active identity after waiting for the runtime lock")
    if logon_startup_cpp != config_profiles_ui_cpp:
        require_text(main_shell_cpp, '#include "main_startup_profiles.cpp"',
                     "split logon-startup shard is compiled into the Windows shell")
    if logon_coordinator_cpp != sessions_cpp:
        require_text(main_shell_cpp, '#include "main_service_lifecycle_events.cpp"',
                     "lifecycle event/inbox shard is compiled into the Windows shell")
        require_text(main_shell_cpp, '#include "main_service_lifecycle_apply.cpp"',
                     "lifecycle apply shard is compiled into the Windows shell")
        require_text(main_shell_cpp, '#include "main_service_logon_coordinator.cpp"',
                     "split service logon coordinator shard is compiled into the Windows shell")
    require_text(service_server_aggregate_cpp,
                 '#include "main_service_request_policy.cpp"',
                 "service request-policy shard is compiled through the server aggregate")
    require_text(service_server_aggregate_cpp,
                 '#include "main_service_pipe.cpp"',
                 "service pipe shard is compiled through the server aggregate")
    require_text(service_server_aggregate_cpp,
                 '#include "main_service_host.cpp"',
                 "service host shard is compiled through the server aggregate")
    require_text(main_service_runtime_aggregate_cpp,
                 '#include "main_service_runtime_identity.cpp"',
                 "service identity/recovery-monitor shard is compiled through the runtime aggregate")
    require_text(main_service_runtime_aggregate_cpp,
                 '#include "main_service_fan_worker.cpp"',
                 "service fan-worker shard is compiled through the runtime aggregate")
    require_text(main_service_runtime_aggregate_cpp,
                 '#include "main_service_apply_runtime.cpp"',
                 "service apply/reset shard is compiled through the runtime aggregate")
    require_text(service_ipc_aggregate_cpp,
                 '#include "main_service_connection.cpp"',
                 "service connection shard is compiled through the IPC aggregate")
    require_text(service_ipc_aggregate_cpp,
                 '#include "main_service_client_commands.cpp"',
                 "typed service-command shard is compiled through the IPC aggregate")
    require_text(service_ipc_aggregate_cpp,
                 '#include "main_service_admin_client.cpp"',
                 "service admin-client shard is compiled through the IPC aggregate")
    require_text(service_ipc_aggregate_cpp,
                 '#include "main_service_machine_config.cpp"',
                 "shared machine-config shard is compiled through the IPC aggregate")

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
    require_text(service_server_cpp, "Your administrator restricts this PC to shared profiles", "service rejects non-admin custom OC under the policy")
    require_text(service_server_cpp, "SERVICE_REQUEST_FLAG_SHARED_SLOT", "service applies its own copy of the named shared slot")
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
    require_text(sessions_cpp, "resolve_logon_profile_source", "service logon resolver uses the shared policy decision")
    require_text(sessions_cpp, "logon_shared_slot", "service logon resolver honors the per-user shared-logon choice")
    require_text(sessions_cpp, "get_machine_restrict_policy", "service logon resolver checks the shared-only policy")
    require_text(logon_coordinator_cpp, "service_auto_restore_is_locked_out", "service owns and safety-gates logon profile application")
    # The per-account logon choice (incl. admin shared profiles) lives in the single
    # unified "Apply profile after user log in" dropdown, tagged via CB_SETITEMDATA;
    # picking a shared entry sets logon_shared_slot and clears logon_slot.
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "LOGON_COMBO_SHARED_FLAG", "Logon dropdown offers admin shared profiles via item-data tags")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"), "CB_GETITEMDATA", "Logon dropdown handler decodes the selected item's meaning from item data")
    require_text(config_utils_cpp, "update_logon_profile_selection_transaction", "logon slot keys use one locked transaction")
    require_text(config_utils_cpp, '"Global\\\\GreenCurveConfigMutex-v2"',
                 "config lock spans GUI and service WTS sessions")
    forbid_text(config_utils_cpp, '"Local\\\\GreenCurveConfigMutex"',
                "session-local mutex cannot protect GUI/service INI access")
    require_text(config_utils_cpp, "CreateMutexExA", "config mutex requests only explicit synchronization rights")
    require_text(config_utils_cpp, "SYNCHRONIZE | MUTEX_MODIFY_STATE",
                 "config mutex grants only wait/release rights")
    require_text(config_utils_cpp, "S:(ML;;NW;;;ME)",
                 "SYSTEM-created config mutex remains accessible to the medium-integrity GUI")
    require_text(config_utils_cpp, "if (!mutex) {\n        LeaveCriticalSection(&g_configLock);",
                 "config lock creation/open failure fails closed")
    require_text_in_operation(config_utils_cpp,
                              "bool update_logon_profile_selection_transaction",
                              "WritePrivateProfileStringA(nullptr, nullptr, nullptr, path)",
                              "atomic logon selection flushes the Win32 INI cache before readback")
    require_text(secure_write_cpp, "config_section_header_matches_ascii",
                 "atomic section replacement follows case-insensitive Win32 INI semantics")
    require_text_in_operation(config_profiles_cpp,
                              "static bool load_profile_from_config",
                              "ConfigStorageLockGuard storageLock;",
                              "profile load holds one cross-session transaction across every field")
    require_text(config_profiles_cpp,
                 "profile_should_strip_legacy_unlocked_curve",
                 "unlocked explicit VF points are distinguished from legacy captured curves")
    require_text_in_operation(config_profiles_cpp,
                              "static bool clear_profile_from_config",
                              "_stricmp(p, targetControls)",
                              "profile clear follows case-insensitive Win32 section semantics")
    require_text_in_operation(config_profiles_cpp,
                              "static bool save_profile_to_config",
                              "ConfigStorageLockGuard storageLock;",
                              "profile save holds the cross-session lock for its whole-file transaction")
    require_text_in_operation(config_profiles_cpp,
                              "static bool clear_profile_from_config",
                              "ConfigStorageLockGuard storageLock;",
                              "profile clear holds the cross-session lock for its whole-file transaction")
    require_text_in_operation(config_profiles_cpp,
                              "static bool clear_profile_from_config",
                              "bool ok2 = !truncated;",
                              "profile clear rejects a truncated serialization before replacing the config")
    forbid_text_in_operation(config_profiles_cpp,
                             "static bool save_profile_to_config",
                             "leave_config_storage_lock(configMutex)",
                             "profile save cannot release the storage lock before its atomic rename")
    require_text(config_profiles_ui_cpp, "commit_logon_profiles_section", "production logon selection uses an atomic whole-section commit")
    require_text(config_profiles_ui_cpp, "select_logon_combo_item_by_data", "logon combo restores selections by item data")
    require_text(startup_task_runtime_cpp, "logon_combo_item_data_from_slots", "async task synchronization preserves shared combo selections")
    require_text(startup_task_runtime_cpp, "startupSyncGeneration", "async startup synchronization is generation-checked")
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"),
                 "completedGeneration != currentGeneration",
                 "stale async startup repair schedules current-state reconciliation")
    require_text(config_profiles_ui_cpp, "Shared profile %d (admin)", "Logon dropdown lists admin-published shared profiles")
    require_text(config_profiles_ui_cpp, "Admin default: Shared profile %d", "Logon dropdown shows the effective all-users default when the account has no choice")
    forbid_text(logon_startup_cpp, "apply_logon_shared_slot_if_configured", "scheduled GUI handoff must not load or apply a logon profile")
    forbid_text_in_operation(entry_cpp, "if (opts.logonStart)", "opts.applyConfig = true;", "CLI scheduled logon must not apply a profile directly")

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
    require_text(service_server_cpp, "response.message[ARRAY_COUNT(response.message) - 1] = '\\0'", "defensive response NUL termination exists")

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
    require_text(main_tail_diagnostics_cpp, "diagnostic only, NO reapply", "runtime tail drift is logged as expected NVIDIA drift, NOT actively reapplied (0.18)")
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

    # A current proof must be invalidated durably immediately before the first
    # possible hardware write. Validation failures stay zero-write; once the
    # attempted bit is published, any failure is terminal for auto restoration.
    require_text_count(gpu_backend_apply_cpp,
        "service_invalidate_oc_apply_proof_before_write()", 2,
        "service apply has one proof boundary for reset-first and one for non-reset writes")
    require_order_in_operation(gpu_backend_apply_cpp,
        "if (desired->resetOcBeforeApply)",
        "service_invalidate_oc_apply_proof_before_write()",
        "if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;",
        "reset-before-apply invalidates proof before publishing a hardware attempt")
    require_order_in_operation(gpu_backend_apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "if (!proofInvalidatedForWrite &&\n        !service_invalidate_oc_apply_proof_before_write())",
        "if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;",
        "non-reset apply invalidates proof after preflight and before its first write")
    require_order_in_operation(main_service_apply_runtime_cpp,
        "static bool service_reset_all(",
        "service_invalidate_oc_apply_proof_before_write()",
        "if (hardwareWriteAttemptedOut) *hardwareWriteAttemptedOut = true;",
        "explicit reset invalidates proof before its first hardware write")

    require_order_in_operation(gpu_backend_apply_cpp,
        "static bool apply_desired_settings_service(const DesiredSettings* desired",
        "service_request_replaces_lock_domain(desired)",
        "nvml_reset_gpu_locked_clocks(",
        "locked clocks reset only after the request is classified as owning the VF/lock domain")
    forbid_text(gpu_backend_apply_cpp,
        "if (lockMode != LOCK_MODE_HARD && g_nvml_api.resetGpuLockedClocks)",
        "sparse fan/memory/power apply must not unconditionally reset a hard clock lock")

    # Diagnostics for rare failure modes (no behavior change).
    require_text(config_utils_cpp, "config mutex was abandoned", "abandoned config mutex acquisitions are logged")
    require_text(service_server_cpp, "lifecycle worker startup failed", "lifecycle worker creation failure fails service startup closed and is logged")

    # GUI repaint robustness during service start/restart (visual corruption).
    require_text(os.path.join(SOURCE_DIR, "ui_main_control_lifecycle.cpp"), "WM_SETREDRAW, FALSE", "edit-control rebuild suppresses painting to avoid partial-paint corruption")
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
    require_text(service_server_cpp, "g_serviceStopEvent) {", "service stop event creation check exists")

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
    linux_daemon_state_cpp = os.path.join(SOURCE_DIR, "linux_daemon_state.cpp")
    linux_service_install_cpp = os.path.join(SOURCE_DIR, "linux_service_install.cpp")
    linux_gpu_selection_h = os.path.join(SOURCE_DIR, "linux_gpu_selection.h")
    linux_transaction_h = os.path.join(SOURCE_DIR, "linux_transaction.h")
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
    require_text(linux_daemon_cpp, "LINUX_DAEMON_RECORD_PREPARED",
                 "Linux daemon journals intent before hardware mutation")
    require_text(linux_daemon_cpp, "LINUX_DAEMON_RECORD_ACTIVE",
                 "Linux daemon publishes only committed restart-reapply state")
    require_text(linux_daemon_cpp, "LINUX_DAEMON_RECORD_UNCERTAIN",
                 "Linux daemon locks out uncertain hardware state")
    require_text(linux_daemon_state_cpp, "renameat",
                 "Linux daemon state commits by same-directory atomic rename")
    require_text(linux_daemon_state_cpp, "fsync(dirfd)",
                 "Linux daemon state rename is made directory-durable")
    require_text(linux_daemon_state_cpp, "O_NOFOLLOW",
                 "Linux daemon state does not follow file/directory symlinks")
    require_text(linux_daemon_state_cpp, "st.st_nlink == 1",
                 "Linux daemon state rejects linked or weakly-permissioned records")
    require_text(linux_backend_cpp, "linux_execute_transaction",
                 "Linux Apply/Reset use the pure ordered transaction engine")
    require_text(linux_transaction_h, "result.rollbackSucceeded",
                 "Linux phase failure exposes verified versus uncertain rollback")
    require_text(linux_gpu_selection_h, "Never compare the low vendor word",
                 "Linux PCI matching cannot collapse every NVIDIA device to vendor ID 10DE")
    require_text(linux_backend_cpp, "nvapiAssigned",
                 "Linux NvAPI handles map at most once to an NVML adapter")
    require_text(linux_backend_cpp, "multi-GPU write target is not proven across NVML and NvAPI",
                 "Linux multi-GPU cross-API mismatch blocks writes")
    require_text(linux_main_cpp, "Cannot validate GPU selection: exact BDF/PCI identity",
                 "Linux CLI persists only a daemon-enriched stable GPU identity")
    require_text(linux_daemon_cpp, "startup reapply", "Linux daemon reapplies settings on (re)start")
    require_text(linux_daemon_cpp, "GC_DAEMON_IO_TIMEOUT_MS", "Linux daemon socket I/O has bounded deadlines")
    require_text(linux_daemon_cpp, "wait_fd_ready", "Linux daemon socket reads/writes poll with a deadline")
    require_text(linux_daemon_cpp, "set_nonblocking(conn)", "Linux daemon accepted clients are nonblocking")
    require_text(linux_daemon_cpp, "linux_daemon_state_remove", "Linux reset durably removes committed state")
    require_text(linux_service_install_cpp, "GC_INSTALL_BIN", "Linux systemd unit uses a protected staged daemon binary")
    require_text(linux_service_install_cpp, "root_owned_nonwritable_path", "Linux service install validates root-owned non-writable parents")
    require_text(linux_service_install_cpp, "stage_service_binary", "Linux service install stages the daemon binary before writing the unit")
    require_text(linux_service_install_cpp, "ExecStart=%s --daemon", "Linux systemd unit launches the staged daemon path")
    require_text(linux_service_install_cpp, "run_root_command", "Linux service management avoids shell command execution")
    forbid_text(linux_service_install_cpp, "system(", "Linux service management never invokes a shell")
    require_text(linux_daemon_cpp, "int connectErrno = 0", "Linux daemon client preserves socket connection errno")
    require_text(linux_daemon_cpp, "connectErrno == EACCES || connectErrno == EPERM", "Linux daemon client identifies socket permission denial")
    require_text(linux_daemon_cpp, "sudo usermod -aG greencurve", "Linux socket permission denial explains group enrollment")
    require_text(linux_main_cpp, "The daemon socket permits root and greencurve group members", "Linux service install explains socket authorization")
    require_text(linux_main_cpp, "sudo usermod -aG greencurve", "Linux help and generated assets document group enrollment")
    # F-LNX-TUI: root-cause fix for the reported mouse-offset bug + keyboard/daemon parity.
    linux_tui_cpp = os.path.join(SOURCE_DIR, "linux_tui.cpp")
    linux_tui_layout_cpp = os.path.join(SOURCE_DIR, "linux_tui_layout.cpp")
    forbid_text(linux_tui_cpp, "Daemon not running. Install it with", "Linux TUI preserves detailed daemon errors instead of masking permission denial")
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
    require_text(linux_backend_cpp, "VF curve write preflight failed",
                 "implausible VF requests fail during preflight with zero writes")
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
    require_text(build_script, "architecture mismatch:", "packaging aborts on a cross-arch bundle mismatch")
    require_text(build_script, "def _verify_archive_manifest", "completed release archives are read back against an exact manifest")
    require_text(build_script, "unexpected linker side product", "packaging regression rejects Zig main.lib")
    require_text(build_script, "ARM64 branch protection missing", "final ARM64 binaries must contain BTI and PAC/AUT")
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
    require_text(service_server_cpp, "RegisterServiceCtrlHandlerExW", "service uses Ex control handler for power events")
    require_text(service_server_cpp, "SERVICE_ACCEPT_POWEREVENT", "service accepts power events")
    require_text(service_server_cpp, "PBT_APMRESUMEAUTOMATIC", "service handles resume from standby")
    require_text(service_server_cpp, "PBT_APMRESUMECRITICAL", "service handles critical standby resume")
    require_text(service_server_cpp, "service_lifecycle_post_resume", "resume handler posts one full-intent lifecycle restore")
    forbid_text(service_server_cpp, "service_resume_reapply_thread_proc", "resume handler must not allocate a per-event thread")

    # FP-01-002: one protected, deduplicated recovery ledger owns persistent
    # driver/TDR spam detection. The lifecycle reducer authorizes one full-intent
    # write; it never infers recovery from live curve drift.
    require_text(main_service_recovery_ledger_cpp, "RECOVERY_LOOP_WINDOW_MS",
        "TDR recovery ledger window exists")
    require_text(main_service_recovery_ledger_cpp, "MAX_RECOVERIES_BEFORE_BACKOFF",
        "TDR recovery ledger threshold exists")
    require_text(main_service_recovery_ledger_cpp, "Global\\\\GreenCurve-RecoveryLedger-v1",
        "recovery ledger is serialized across service/helper processes")
    require_text(main_service_recovery_ledger_cpp, "service_recovery_evidence_already_recorded",
        "corroborating recovery observations are deduplicated")
    require_text(main_service_logon_coordinator_cpp, "service_lifecycle_attempt_driver_restore",
        "long-lived lifecycle worker owns driver restoration")
    require_text(main_service_logon_coordinator_cpp, "full-intent write attempted=",
        "driver restoration logs its sole terminal hardware write")

    # FP-01-003: GPU-driver-restart recovery must not use the old ad-hoc fan
    # pulse NVML re-init path (which crash-looped/hung).  See FP-06 for the
    # restart-based recovery; assert the old in-process re-init path is gone.
    forbid_text(main_service_runtime_cpp, "NVML stale, attempting recovery",
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

    # F-DRIFT-1: VF boost/temperature drift is telemetry only and must never leak
    # into the editor, graph, fan-only apply classification, or a saved profile.
    # The single source for owned VF points is the drift-free applied-intent
    # baseline g_app.appliedCurveMHz, populated ONLY from intent (DesiredSettings),
    # never from live g_app.curve readback.
    require_text(os.path.join(SOURCE_DIR, "app_shared.h"),
        "unsigned int appliedCurveMHz[VF_NUM_POINTS];",
        "F-DRIFT-1: drift-free applied VF curve intent baseline exists in AppState")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "static void capture_applied_curve_baseline(const DesiredSettings* desired)",
        "F-DRIFT-1: baseline is captured from intent (DesiredSettings), not live readback")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "g_app.appliedCurveMHz[i] = desired->curvePointMHz[i];",
        "F-DRIFT-1: baseline values come from the applied desired curve, not g_app.curve")
    # Fan-only apply detection compares the editor against the drift-free baseline,
    # NOT live readback, so expected boost drift on a pre-tail point can no longer
    # reclassify a fan-only change as a curve edit (the reported bug).
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "unsigned int baselineMHz = g_app.appliedCurveMHz[i];",
        "F-DRIFT-1: fan-only curve-change detection uses the drift-free baseline")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "baselineMHz == 0 || full.curvePointMHz[i] != baselineMHz",
        "F-DRIFT-1: a newly-owned point (no baseline) or an edited value still forces a full apply")
    # A fan-only apply carries no curve intent, so it must NOT rewrite the baseline
    # (which would drop the curve the service still holds).
    require_text(os.path.join(SOURCE_DIR, "gpu_backend.cpp"),
        "if (ok && !fanOnlyApply) capture_applied_curve_baseline(desired);",
        "F-DRIFT-1: baseline is refreshed on a real curve apply, preserved on fan-only apply")
    # Adopting the service's active desired keeps the baseline drift-free across
    # reconnects / telemetry refreshes.
    require_text(os.path.join(SOURCE_DIR, "main_state_sync.cpp"),
        "capture_applied_curve_baseline(desired);",
        "F-DRIFT-1: GUI adopts the service active-desired curve as the drift-free baseline")
    # The editor/graph repaint sources owned points from the baseline, not live
    # readback, so drift never surfaces as a displayed configured value.
    require_text(os.path.join(SOURCE_DIR, "ui_main.cpp"),
        "unsigned int ownedMHz = (ci >= 0 && ci < VF_NUM_POINTS) ? g_app.appliedCurveMHz[ci] : 0;",
        "F-DRIFT-1: populate_edits shows owned VF points from the drift-free baseline")
    # Reset-to-stock drops all owned intent so the editor shows live stock values.
    require_text(os.path.join(SOURCE_DIR, "ui_main_window.cpp"),
        "memset(g_app.appliedCurveMHz, 0, sizeof(g_app.appliedCurveMHz));",
        "F-DRIFT-1: reset clears the owned VF curve intent baseline")

    # F-APPLY-SPEED: profile-switch speed levers must be gated with defaults that
    # preserve the exact current (TDR-safe) behaviour — the fast paths are opt-in and
    # require GPU validation. reset_settle_ms defaults to the historical 1000ms;
    # skip_reset_curve_write defaults to 0 (the reset-to-zero write still runs).
    require_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
        "get_config_int(g_app.configPath, \"apply\", \"reset_settle_ms\", 1000)",
        "F-APPLY-SPEED: TDR settle is tunable but defaults to the historical 1000ms")
    # The reset-curve-write is load-bearing for the delta-based selective/boost apply
    # (excluded points collapse to originalCurveOffsets[ci], so a skipped reset strands
    # the previous profile's offset on them). It must NOT be gated/skippable.
    forbid_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
        "\"apply\", \"skip_reset_curve_write\"",
        "F-APPLY-SPEED: the VF-curve reset-to-zero must not be skippable (delta-boost baseline)")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
        "if (hadCurveOffsets && !apply_curve_offsets_verified(resetOffsets, resetMask, 2))",
        "F-APPLY-SPEED: the VF-curve reset-to-zero always runs when the previous profile had curve offsets")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
        "g_app.gpuClockOffsetkHz != 0 && !nvapi_set_gpu_offset(0)",
        "F-APPLY-SPEED: an owned GPU-offset transition resets that offset before the VF curve")

    # F-NO-INJECT: the auto-profile subsystem observes other processes/windows
    # strictly read-only (foreground/window-metadata query + a toolhelp process
    # snapshot + an OUTOFCONTEXT WinEvent hook).  It must NEVER touch another
    # process: no remote memory/thread writes, no in-process DLL hook, and it does
    # not even OPEN A HANDLE to another process (the foreground exe name comes from
    # the global process-list snapshot, not OpenProcess).  Locks the invariant so
    # the tool cannot trip anti-cheat.
    auto_detect_cpp = os.path.join(SOURCE_DIR, "auto_profile_detect.cpp")
    auto_win32_cpp = os.path.join(SOURCE_DIR, "auto_profile_win32.cpp")
    for _apf in (auto_detect_cpp, auto_win32_cpp):
        forbid_text(_apf, "WriteProcessMemory", "F-NO-INJECT: auto-profiles must not write another process's memory")
        forbid_text(_apf, "CreateRemoteThread", "F-NO-INJECT: auto-profiles must not create remote threads")
        forbid_text(_apf, "VirtualAllocEx", "F-NO-INJECT: auto-profiles must not allocate in another process")
        forbid_text(_apf, "OpenProcess", "F-NO-INJECT: auto-profiles must not open a handle to another process")
    require_text(auto_detect_cpp, "CreateToolhelp32Snapshot",
        "F-NO-INJECT: the foreground exe name comes from the global process snapshot, not OpenProcess")
    require_text(auto_win32_cpp, "WINEVENT_OUTOFCONTEXT",
        "F-NO-INJECT: the foreground hook is out-of-context (no DLL injected into other processes)")

    # F-AUTO-PROFILE: the driver is wired into the GUI lifecycle (init/shutdown),
    # the WM_HOTKEY path, and the unity build.
    auto_ui_cpp = os.path.join(SOURCE_DIR, "ui_main_window.cpp")
    auto_rules_cpp = os.path.join(SOURCE_DIR, "auto_profile_rules.cpp")
    require_text(auto_ui_cpp, "auto_profile_init(hwnd);",
        "F-AUTO-PROFILE: subsystem is initialized on main-window WM_CREATE")
    require_text(auto_ui_cpp, "auto_profile_shutdown(hwnd);",
        "F-AUTO-PROFILE: subsystem is torn down on WM_DESTROY (hook/hotkeys/timers)")
    require_text(auto_ui_cpp, "auto_profile_on_hotkey(hwnd, (int)wParam);",
        "F-AUTO-PROFILE: global hotkeys route through WM_HOTKEY")
    require_text(os.path.join(SOURCE_DIR, "main.cpp"), '#include "auto_profile_win32.cpp"',
        "F-AUTO-PROFILE: the driver is compiled into the GUI unity build")
    require_text(auto_rules_cpp, "write_config_sections_atomic",
        "F-AUTO-PROFILE: rules and hotkeys commit through one atomic whole-file transaction")
    require_text(auto_rules_cpp, "const char (*hotkeys)[64]",
        "F-AUTO-PROFILE: hotkeys participate in the same transaction as rules")

    # F-DIAG-OFFSET: offset-convergence diagnostics so a driver that accepts a VF
    # offset write but reports back a different offset (forcing ~1s retry writes,
    # e.g. the +475 MHz boost tip at point 127) is visible in the debug log.
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "curve batch pass %d unconverged: ci=%d wroteOffset=%dkHz readbackOffset=%dkHz driverDelta=%dkHz",
        "F-DIAG-OFFSET: per-pass unconverged VF offset points log wrote-vs-readback offset gap")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "curve offset unconverged (final): ci=%d wroteOffset=%dkHz readbackOffset=%dkHz",
        "F-DIAG-OFFSET: final unconverged VF offset summary logs the driver-honored offset")

    # F-OFFSET-REFUSAL: a populated-but-placeholder VF point that the driver pins at
    # offset 0 (wrote non-zero, readback stays exactly 0) is accepted as
    # non-offsettable instead of being retried across passes + a per-point fallback
    # (~1s NVAPI setControl each). This is what made a selective-offset+flatten apply
    # exceed 5s (the +475 boost tip at point 127 could never converge). Mirrors the
    # existing verify-time "hardware refused offset ... accepting actual" acceptance.
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "bool driverRefused[VF_NUM_POINTS] = {};",
        "F-OFFSET-REFUSAL: driver-refused VF points are tracked so they are not retried")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "desiredOffsets[i] != 0 && g_app.freqOffsets[i] == 0",
        "F-OFFSET-REFUSAL: refusal detected when a non-zero offset write leaves readback pinned at 0")
    require_text(os.path.join(SOURCE_DIR, "main_runtime_gpu.cpp"),
        "accepting as non-offsettable placeholder",
        "F-OFFSET-REFUSAL: refused placeholder points are accepted (not retried) to keep apply fast")

    # F-RESET-INTENT: service_reset_all must drop the active-desired intent BEFORE the
    # post-reset state refresh + control/snapshot population. Otherwise
    # detect_locked_tail_from_curve() preserves the old lock and
    # initialize_gui_fan_settings_from_live_state() re-reads the stale desired fan
    # mode into g_app.activeFanMode, so the RESET reports the old Custom Curve fan
    # mode / lock and the GUI re-adopts them (regression: reset shows Custom Curve).
    require_text(main_service_runtime_cpp,
        "F-RESET-INTENT",
        "F-RESET-INTENT: reset drops active-desired intent before post-reset refresh")
    require_order(main_service_runtime_cpp,
        "F-RESET-INTENT",
        "Clear persisted runtime state BEFORE refreshing",
        "F-RESET-INTENT: active-desired is cleared before refresh_global_state in service_reset_all")
    require_order(main_service_runtime_cpp,
        "F-RESET-INTENT",
        "initialize_gui_fan_settings_from_live_state(false)",
        "F-RESET-INTENT: active-desired is cleared before post-reset fan derivation")

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

    # FP-03-004: interface-specific PnP callbacks only coalesce a readiness cue;
    # the long-lived lifecycle worker performs all probing and writing.
    require_text(service_server_cpp,
        "service_lifecycle_post_prerequisite_signal",
        "device arrival/removal posts lifecycle readiness instead of writing")

    # FP-03-005: Device arrival no longer re-inits NVML/NVAPI directly on the
    # SCM control thread; it requests a service restart via the chokepoint (FP-06).
    forbid_text(service_server_cpp,
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
    # profile, launch the process-bound helper, report a clean stop, and let that
    # helper demand-start the fresh (clean-DLL) process only after SCM confirms
    # SERVICE_STOPPED.

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
        "service_lifecycle_post_prerequisite_signal",
        "display-adapter notifications coalesce lifecycle readiness state")
    require_text(service_host_cpp,
        "service_emergency_restart_from_poisoned_runtime(\n                        \"fan pulse wedged inside nvml.dll\", true);",
        "fan-pulse wedge watchdog closes the hardware gate and requests durable controlled recovery")

    # FP-06-003: request_service_restart commits only after the protected helper
    # validates its inherited parent handle. The old process reports STOP_PENDING
    # and exits with the dedicated code; SCM failure actions cannot race it.
    require_text(main_cpp,
        "static void request_service_restart(const char* reason) {",
        "request_service_restart exists")
    require_text(main_cpp, "service_prepare_controlled_restart(reason",
        "restart request prepares helper before committing")
    require_text(service_server_cpp,
        "InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0",
        "service_main has a driver-recovery restart-exit branch")
    require_text(service_server_cpp, "SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);",
        "controlled restart reports a clean service stop")
    require_text(service_server_cpp, "ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);",
        "controlled restart uses the helper-only dedicated exit code")
    require_order(service_server_cpp,
        "ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);",
        "service_reset_all(resetDetail",
        "controlled restart exit precedes normal NVML teardown")

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

    # FP-06-005 / F-BUG-016/F-BUG-017: process-bound recovery requires a nonce-
    # bound helper and the old service's dedicated clean exit.  Snapshot-only,
    # ordinary SCM, Task Manager, crash, and stale-nonce starts stay idle.
    require_text(main_service_persist_cpp, "ServiceRestartReapplySnapshot",
        "controlled snapshot persists complete intent and ownership")
    require_text(main_service_controlled_restart_cpp, "--recovery-restart-helper",
        "minimal recovery restart helper has an internal entry path")
    require_text(main_service_controlled_restart_cpp, "--controlled-recovery",
        "helper passes a nonce-bound service argument")
    require_text(main_service_controlled_restart_cpp,
        "PROC_THREAD_ATTRIBUTE_HANDLE_LIST",
        "helper inherits only synchronized protocol handles")
    require_text(main_service_controlled_restart_cpp,
        "SERVICE_CONTROLLED_RECOVERY_EXIT_CODE",
        "helper accepts only the dedicated parent exit code")
    require_text(main_service_controlled_restart_cpp,
        "NotifyServiceStatusChangeW",
        "controlled helper waits for SCM state through status notifications")
    require_text(main_service_controlled_restart_cpp,
        "SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_DELETE_PENDING",
        "controlled helper waits for STOPPED and rejects service deletion")
    require_text(main_service_controlled_restart_cpp,
        "WaitForSingleObjectEx(",
        "controlled helper uses an alertable synchronization wait without polling or sleeps")
    require_text(main_service_controlled_restart_cpp,
        "service_classify_controlled_recovery_scm_stop_state(",
        "controlled helper classifies SCM stop transitions without trusting STOP_PENDING process IDs")
    require_text(main_service_controlled_restart_cpp,
        "status.dwCurrentState == SERVICE_STOP_PENDING",
        "controlled helper recognizes SCM's process-ID-ambiguous STOP_PENDING state")
    require_text(main_cpp,
        "InitializeCriticalSection(&g_debugLogLock);",
        "service helper initializes serialized diagnostics before dispatch")
    require_order(main_cpp,
        "InitializeCriticalSection(&g_debugLogLock);",
        "service_try_dispatch_controlled_restart_helper(&helperExitCode)",
        "service helper diagnostics are initialized before helper dispatch")
    require_text(main_service_controlled_restart_cpp,
        "Keep the exact old process object open through SCM generation",
        "controlled helper pins the old process identity against PID reuse through restart")
    require_order_in_operation(main_service_controlled_restart_cpp,
        "static bool service_start_from_controlled_helper",
        "service_wait_for_scm_stopped_notification(",
        "service_read_controlled_recovery_authorization(",
        "controlled helper waits for STOPPED before its final authorization validation")
    require_order_in_operation(main_service_controlled_restart_cpp,
        "static bool service_start_from_controlled_helper",
        "service_read_controlled_recovery_authorization(",
        "StartServiceW(",
        "controlled helper revalidates authorization immediately before its start attempt")
    require_order_in_operation(main_service_controlled_restart_cpp,
        "static void service_emergency_restart_from_poisoned_runtime",
        "g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;",
        "ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);",
        "poisoned-runtime controlled exit leaves authoritative STOPPED publication to SCM")
    require_order_in_operation(service_host_cpp,
        "if (InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0",
        "g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;",
        "ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);",
        "serialized controlled exit leaves authoritative STOPPED publication to SCM")
    with open(main_service_controlled_restart_cpp, "r", encoding="utf-8", errors="replace") as handle:
        controlled_restart_source = handle.read()
    if controlled_restart_source.count("StartServiceW(") != 1:
        print("Regression source check FAILED: controlled helper must contain exactly one StartServiceW call")
        sys.exit(1)
    require_text(main_service_controlled_restart_cpp,
        "QueryServiceDynamicInformation",
        "controlled startup validates the SCM start reason")
    require_text(service_server_cpp,
        "service_prepare_controlled_recovery_startup(argc, argv);",
        "controlled authorization is validated while START_PENDING")
    require_order(service_server_cpp,
        "service_prepare_controlled_recovery_startup(argc, argv);",
        "g_serviceStatus.dwCurrentState = SERVICE_RUNNING;",
        "controlled validation precedes RUNNING")
    require_text(main_service_controlled_restart_cpp,
        "service_clear_controlled_recovery_files();",
        "ordinary, stale, malformed, and helper-failure starts discard replay state")
    forbid_text(main_service_recovery_cpp,
        "service_load_restart_reapply_snapshot",
        "general recovery paths cannot fall back to disk intent")
    require_text(main_service_recovery_ledger_cpp, "service_record_restart_event",
        "restart events use the protected ledger")
    require_text(main_service_recovery_ledger_cpp, "if (!ledgerPathReady)",
        "explicit history clear fails closed when the current ledger path cannot be resolved")
    require_text(main_service_recovery_ledger_cpp, "if (!legacyPathReady)",
        "explicit history clear fails closed when the legacy history path cannot be resolved")
    require_text(main_service_logon_coordinator_cpp,
        "SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM",
        "driver recovery latches persistent spam lockout")
    require_text(main_service_logon_coordinator_cpp,
        "waiting for a real PnP/readiness signal",
        "driver readiness stays pending without polling")
    forbid_text(main_service_controlled_restart_cpp, "Sleep(",
        "controlled restart uses synchronization objects, not timing sleeps")
    forbid_text(main_service_controlled_restart_cpp, "SleepEx(",
        "controlled restart uses alertable synchronization, not timing sleeps")
    forbid_text(main_service_recovery_cpp,
        "service_startup_coordinator_thread_proc",
        "retired per-start recovery thread stays removed")
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
    # F-NO-DRIFT-FIGHT (0.18): the continuous VF-drift monitor + auto-reapply was
    # REMOVED. NVIDIA's VF curve drifts a few MHz with temperature/boost; "correcting"
    # it meant re-applying the whole OC under game load (reset-to-stock spike +
    # aggressive rewrite = TDR risk) and it looped forever on a below-floor flatten
    # target (e.g. 2957 vs a floored 2962). Assert it stays gone. Settings persist ONLY
    # via the event-driven reapply worker (resume-from-standby / driver-TDR recovery /
    # session logon) — never a periodic "is the curve still exactly on target" poll.
    forbid_text(main_cpp, "SERVICE_VF_DRIFT_CHECK_INTERVAL_MS",
        "continuous VF-drift monitor tuning constants must stay removed")
    forbid_text(service_server_cpp, "service_check_active_vf_drift_monitor",
        "service main loop must NOT run a continuous VF-drift monitor / auto-reapply")
    forbid_text(main_shell_cpp, "main_service_vf_drift.cpp",
        "the VF-drift monitor shard must stay removed (no include)")
    require_text(service_server_cpp, "NO continuous VF-drift monitor",
        "the VF-drift monitor removal is documented at the old service-loop call site")
    require_text(main_tail_diagnostics_cpp, "diagnostic only, NO reapply",
        "tail drift diagnostic states drift is expected and NOT reapplied")
    require_text(main_tail_diagnostics_cpp, "s_tailDriftLastLoggedValid",
        "tail drift diagnostics log first/change/reappeared drift instead of every telemetry poll")
    # F-REL-4: OC stabilization window — settings that crash-restart the service
    # within 10 min of being applied are treated as unstable and NOT auto-reapplied
    # (by either reapply method), so an unstable OC cannot loop.  The explicit
    # policy is shared with the logon path and a persistent lockout prevents a
    # later service/OS restart from silently rearming it.
    require_text(os.path.join(SOURCE_DIR, "app_shared.h"), "AUTO_RESTORE_STABILITY_WINDOW_MS",
        "shared 10-minute automatic-restore proving period exists")
    require_text(app_shared_cpp, "should_auto_apply_logon_profile",
        "logon automatic-apply policy is a pure shared decision")
    require_text(app_shared_cpp, "should_auto_restore_after_driver_event",
        "driver-event restoration policy is a pure shared decision")
    require_text(app_shared_cpp, "should_auto_restore_after_standby_resume",
        "standby restoration deliberately bypasses the driver proving period")
    require_text(main_service_persist_cpp, "service_auto_restore_lockout.bin",
        "automatic-restore lockout persists across service restarts")
    require_text(main_service_persist_cpp,
        "AutoRestoreLockoutReason",
        "automatic-restore lockout has a durable protected registry fallback")
    require_text(main_service_persist_cpp,
        "bool durable = fileOk || registryOk;",
        "lockout latching records whether either durable backend succeeded")
    require_text(main_service_recovery_ledger_cpp, "SERVICE_AUTO_RESTORE_LOCKOUT_TDR_SPAM",
        "persistent policy distinguishes TDR/restart spam")
    require_text(main_service_recovery_clock_cpp, "service_auto_restore_allowed_after_driver_event",
        "driver-event restore requires a valid stable apply stamp")
    require_text(main_service_recovery_clock_cpp, "service_capture_mature_oc_apply_proof",
        "standby can preserve an already mature same-boot proof")
    require_text(main_service_recovery_clock_cpp, "SystemBootEnvironmentInformation == 90",
        "driver proof uses the stable per-boot BootIdentifier")
    require_text(main_service_recovery_clock_cpp, "bootIdentifier",
        "driver proof retains the complete Windows BootIdentifier")
    forbid_text(main_service_recovery_clock_cpp, "SystemTimeOfDayInformation == 3",
        "driver proof must not use wall-clock-derived BootTime as a boot identity")
    require_text(service_server_cpp, "service_record_oc_apply_stamp()",
        "successful settings applications record the proving stamp")
    require_text(service_server_cpp, "service_clear_oc_apply_stamp()",
        "reset clears the OC stabilization stamp")
    require_text(service_server_cpp, "service_clear_auto_restore_lockout();",
        "only an explicit successful apply re-arms automatic restoration")
    require_text(main_service_recovery_cpp, "service_disable_automatic_restore",
        "failed/unsafe recovery disables future automatic restoration without a reset")
    require_text(main_service_logon_coordinator_cpp, "service_lifecycle_attempt_standby_restore",
        "standby restore has no 10-minute driver-recovery proving-period gate")
    require_text(main_service_logon_coordinator_cpp, "service_auto_restore_is_locked_out",
        "configured logon profiles are subject to persistent auto-restore lockout")
    require_text(main_service_logon_coordinator_cpp, "service_record_oc_apply_stamp();",
        "successful logon apply begins the driver-event proving period")
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
    require_text(main_service_logon_coordinator_cpp,
        "service_check_disk_version_on_device_arrival",
        "lifecycle PnP readiness logs the on-disk driver version delta")
    require_text(build_py_text,
        '"-lversion"',
        "version.lib linked for GetFileVersionInfoW / VerQueryValueW")

    # FP-06-008: failure actions remain an availability net for unexpected
    # crashes only. Such restarts carry no nonce and are strictly non-restoring;
    # controlled driver recovery uses its helper and clean stop instead.
    require_text(main_service_install_cpp,
        "SERVICE_CONFIG_FAILURE_ACTIONS_FLAG",
        "service retains unexpected-failure availability actions")
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
    require_order_in_operation(service_server_cpp,
        "case SERVICE_CMD_RESET:",
        "service_explicit_supersede_automatic_work_locked(",
        "GPU driver recovery was superseded, but the driver is still transitional",
        "RESET supersedes pending recovery but rejects hardware access while the driver is transitional")

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
            run_check_builds(args.target, args.arch, generate_lsp=not args.sanitizer)
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
