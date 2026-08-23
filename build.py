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
import time
import zlib
from contextlib import nullcontext

# 0.13.0 is a compatibility ceiling, not neglect.  Zig compiles the Windows
# ARM64 target, and from 0.14.1 on its clang reports
# -Wcast-function-type-mismatch for the GetProcAddress casts throughout the
# Windows sources, which -Werror turns into build failures.  0.15.1 and later
# additionally answer "error: unimplemented" for every ELF objcopy operation,
# which breaks Linux private-symbol extraction (tools/crash_artifacts.py).
# Moving up means fixing those call sites first; see compilers/README.md.
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

# GitHub release base for pre-packaged toolchain archives.  Every compiler and
# archiver the build runs comes from here, including the ones upstream also
# publishes themselves: a release that is attested to this repository should
# not depend on a third party still serving a byte-identical file.
COMPILERS_REPO_BASE = "https://github.com/aufkrawall/green-curve/releases/download/Compilers-1.0"
COMPILERS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compilers")

_ZIG_ARCHIVE_NAME = f"zig-{_ZIG_PLATFORM}-x86_64-{ZIG_VERSION}{_ZIG_ARCHIVE_EXT}"
ZIG_URL = f"{COMPILERS_REPO_BASE}/{_ZIG_ARCHIVE_NAME}"
ZIG_SHA256 = _ZIG_SHA256
# Digest of the extracted zig executable (which came from an archive already
# verified against _ZIG_SHA256).  A missing per-host entry fails open and
# re-downloads + re-extracts the whole toolchain on EVERY invocation.
ZIG_EXE_SHA256 = {
    "windows": "2e44af5bbf7a72ef8cbdae370284687c95d65a19affa469d2ad0364d905b8e84",
    "linux": "7f9e3a661e909d5188d1b8b14f082b98a19c323a30d43bfdd1b2893ed37273e0",
}.get(_ZIG_PLATFORM)

# llvm-mingw: portable MinGW toolchain for Windows builds with full CFG support.
# 20260519 is the newest release that builds this project cleanly.  20260616 and
# later ship a clang that reports -Wcast-function-type-mismatch on the 20
# GetProcAddress casts this codebase relies on, and -Werror turns every one of
# them into a build failure; moving up means fixing those call sites, not
# silencing a type-safety diagnostic across the whole build.
LLVM_MINGW_VERSION = "20260519"
if sys.platform == "win32":
    LLVM_MINGW_ARCHIVE_NAME = f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-x86_64.zip"
    LLVM_MINGW_SHA256 = "72dbd6e64614e3b3401998992d1bd9c8ace29e74611d71c80309ea71c3fb26f9"
    LLVM_MINGW_CLANG_SHA256 = "e04c3380970bf64d07074c390f550371dbd12dbb46a263609b11cd164ac1faf8"
    LLVM_MINGW_ARCHIVE_EXT = ".zip"
    LLVM_MINGW_TOOL_SUFFIX = ".exe"
    LLVM_MINGW_CLANG_NAME = "clang++.exe"
else:
    LLVM_MINGW_ARCHIVE_NAME = \
        f"llvm-mingw-{LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz"
    LLVM_MINGW_SHA256 = "a48f8c2801508272ccde64d87a26747ecc5306623d9a080a42ed80dc61f79fa2"
    # The Linux driver is a symlink to a 3 KB wrapper script that is unchanged
    # across upstream releases, so this digest alone cannot tell one llvm-mingw
    # from another.  toolchain.verify_tree() checks the real binaries too.
    LLVM_MINGW_CLANG_SHA256 = "c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f"
    LLVM_MINGW_ARCHIVE_EXT = ".tar.xz"
    LLVM_MINGW_TOOL_SUFFIX = ""
    LLVM_MINGW_CLANG_NAME = "x86_64-w64-mingw32-clang++"
LLVM_MINGW_URL = f"{COMPILERS_REPO_BASE}/{LLVM_MINGW_ARCHIVE_NAME}"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Fuzzing and CET gates live in tools/ so this script stays under its size
# ratchet.  The dependency runs one way: security_gates never imports build.py.
sys.path.insert(0, os.path.join(SCRIPT_DIR, "tools"))
import security_gates  # noqa: E402  (needs SCRIPT_DIR on sys.path first)
import ui_gates  # noqa: E402  (same one-way dependency as security_gates)
import fan_gates  # noqa: E402  (same one-way dependency as security_gates)
import xbar_gates  # noqa: E402  (same one-way dependency as security_gates)
import linux_gates  # noqa: E402  (same one-way dependency as security_gates)
import update_gates  # noqa: E402  (same one-way dependency as security_gates)
import icon_render  # noqa: E402  (same one-way dependency as security_gates)
import crash_artifacts  # noqa: E402  (same one-way dependency as security_gates)
import driver_inspect  # noqa: E402  (same one-way dependency as security_gates)
import static_analysis  # noqa: E402  (same one-way dependency as security_gates)
import build_scheduler  # noqa: E402  (same one-way dependency as security_gates)
import build_state  # noqa: E402  (same one-way dependency as security_gates)
import toolchain  # noqa: E402  (same one-way dependency as security_gates)
from release_manifest import (  # noqa: E402  (one-way dependency)
    RUNTIME_ARTIFACT_NAMES, check_all as release_manifest_check_all,
    check_packaging_skip_warning, expected_release_names, find_seven_zip,
    purge_runtime_artifacts, release_archive_extension, release_archive_paths,
    release_archive_root, report_packaging_skipped, stage_release_file,
    validate_payload_file_names, verify_linux_tarball, verify_seven_zip_manifest,
    write_linux_tarball)
import installer_build  # noqa: E402  (same one-way dependency as security_gates)

FUZZ_TARGETS = security_gates.FUZZ_TARGETS

ZIG_DIR = os.path.join(SCRIPT_DIR, "zig")
ZIG_EXE = os.path.join(ZIG_DIR, _ZIG_EXE_NAME)
LLVM_MINGW_DIR = os.path.join(
    SCRIPT_DIR, "llvm-mingw" if sys.platform == "win32" else "llvm-mingw-linux")
LLVM_MINGW_CLANG = os.path.join(LLVM_MINGW_DIR, "bin", LLVM_MINGW_CLANG_NAME)
LLVM_MINGW_RC = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-rc" + LLVM_MINGW_TOOL_SUFFIX)
LLVM_MINGW_OBJCOPY = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-objcopy" + LLVM_MINGW_TOOL_SUFFIX)
LLVM_MINGW_STRIP = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-strip" + LLVM_MINGW_TOOL_SUFFIX)
LLVM_MINGW_READOBJ = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-readobj" + LLVM_MINGW_TOOL_SUFFIX)
LLVM_MINGW_PDBUTIL = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-pdbutil" + LLVM_MINGW_TOOL_SUFFIX)
LLVM_MINGW_NM = os.path.join(LLVM_MINGW_DIR, "bin", "llvm-nm" + LLVM_MINGW_TOOL_SUFFIX)
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
    os.path.join(SOURCE_DIR, "config_text_utils.cpp"),
    os.path.join(SOURCE_DIR, "fan_curve.cpp"),
    os.path.join(SOURCE_DIR, "ssp_glue.cpp"),
    os.path.join(SOURCE_DIR, "cfg_glue.cpp"),
    os.path.join(SOURCE_DIR, "service_acl.cpp"),
    os.path.join(SOURCE_DIR, "platform_win32.cpp"),
    os.path.join(SOURCE_DIR, "vf_backends.cpp"),
]


LINUX_SOURCE_FILES = [
    os.path.join(SOURCE_DIR, "linux_main.cpp"),
    os.path.join(SOURCE_DIR, "linux_cli_options.cpp"),
    os.path.join(SOURCE_DIR, "linux_debug_log.cpp"),
    # Startup half of the crash report: path resolution/rotation/descriptor.
    os.path.join(SOURCE_DIR, "linux_crash_report.cpp"),
    os.path.join(SOURCE_DIR, "linux_terminal_launch.cpp"),
    os.path.join(SOURCE_DIR, "linux_port.cpp"),
    os.path.join(SOURCE_DIR, "linux_port_profiles.cpp"),
    os.path.join(SOURCE_DIR, "linux_profile_output.cpp"),
    os.path.join(SOURCE_DIR, "linux_live_output.cpp"),
    # Client half of the boot-apply snapshot invariant, shared by the TUI and
    # the CLI so a profile write can never leave the daemon booting old values.
    os.path.join(SOURCE_DIR, "linux_startup_sync.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_actions.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_render.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_layout.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_layout_vf.cpp"),
    os.path.join(SOURCE_DIR, "linux_tui_layout_fan_profiles.cpp"),
    os.path.join(SOURCE_DIR, "linux_gpu.cpp"),
    os.path.join(SOURCE_DIR, "linux_backend.cpp"),
    os.path.join(SOURCE_DIR, "linux_daemon.cpp"),
    os.path.join(SOURCE_DIR, "linux_daemon_state.cpp"),
    os.path.join(SOURCE_DIR, "platform_posix.cpp"),
    os.path.join(SOURCE_DIR, "vf_backends.cpp"),
    # Shared text/fan-curve helpers.  linux_port.cpp carried private copies
    # until 2026-07-28; its fan_curve_normalize wrote past the 8-point array.
    os.path.join(SOURCE_DIR, "config_text_utils.cpp"),
    os.path.join(SOURCE_DIR, "fan_curve.cpp"),
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
TRAY_ICON_PENDING_ICO = os.path.join(SCRIPT_DIR, "greencurve_tray_pending.ico")
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
    ("tray_pending", TRAY_ICON_PENDING_ICO, (64, 48, 32, 24, 16)),
]

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
    "-flto",
    # Dynamically linked against glibc so the backend can dlopen the NVIDIA
    # driver libraries at runtime (see LINUX_TARGET note).  Hardening kept.
    "-fPIE",
    "-pie",
    "-Wl,-z,relro,-z,now",
    "-Wl,-z,noexecstack",
    # Debug info is EMITTED here and split out post-link by
    # crash_artifacts.extract_linux_symbols(); the shipped binary is still fully
    # stripped.  LINUX_FLAGS used to carry a bare "-s" with no extraction, so a
    # Linux core — or the crash report's PC — could not be symbolized at all.
    # The prefix maps keep the private workspace path out of the shipped binary.
    "-g",
    "-gdwarf-4",
    f"-ffile-prefix-map={SCRIPT_DIR}=.",
    f"-fdebug-prefix-map={SCRIPT_DIR}=.",
    "-fdebug-compilation-dir=.",
    # A build-id is what lets coredumpctl/gdb match a core to the .debug file
    # without being told where it is.
    "-Wl,--build-id=sha1",
    "-fstack-protector-strong",
    # CET: x86-only, so linux_flags_for_arch() drops it for aarch64 (which
    # gets -mbranch-protection=standard instead).  This marks our own objects
    # with GNU_PROPERTY_X86_FEATURE_1_{IBT,SHSTK} and emits endbr64; the final
    # link still drops the note because the bundled Zig CRT objects carry no
    # property and lld intersects the feature sets.  See llm-wiki/build.md —
    # forcing it is not available here, `zig cc` rejects both -z shstk and
    # -z force-ibt as unsupported linker extension flags.
    "-fcf-protection=full",
    # Matches the Windows build: uninitialised locals get a pattern rather
    # than whatever was on the stack.
    "-ftrivial-auto-var-init=pattern",
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
    "-lbcrypt",
    # The in-app updater's HTTP client.  Linked into both binaries because they
    # compile the same translation units; only the service ever runs it.
    "-lwinhttp",
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
    "-lbcrypt",
    # The in-app updater's HTTP client.  Linked into both binaries because they
    # compile the same translation units; only the service ever runs it.
    "-lwinhttp",
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
        # -fcf-protection is x86-only; clang hard-errors with "option
        # 'cf-protection=return' cannot be specified on this target" on
        # aarch64, so it is removed rather than merely unused.
        flags.remove("-fcf-protection=full")
        flags.remove("-flto")
        flags.append("-fno-lto")
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


def configure_build_number(bump_for_real_build):
    global APP_BUILD_NUMBER
    APP_BUILD_NUMBER = build_state.read_int_file(BUILD_NUMBER_PATH, 0)
    fingerprint = build_state.compute_build_fingerprint(SCRIPT_DIR, SOURCE_DIR)
    previous = ""
    try:
        with open(BUILD_FINGERPRINT_PATH, "r", encoding="utf-8") as handle:
            previous = handle.read().strip()
    except OSError:
        previous = ""
    if bump_for_real_build and fingerprint != previous:
        APP_BUILD_NUMBER += 1
        build_state.write_text_if_changed(BUILD_NUMBER_PATH, f"{APP_BUILD_NUMBER}\n")
        build_state.write_text_if_changed(BUILD_FINGERPRINT_PATH, f"{fingerprint}\n")
        print(f"Build number bumped to {APP_BUILD_NUMBER}")
    elif bump_for_real_build and not os.path.exists(BUILD_FINGERPRINT_PATH):
        build_state.write_text_if_changed(BUILD_FINGERPRINT_PATH, f"{fingerprint}\n")
    COMMON_FLAGS.append(f"-DAPP_BUILD_NUMBER={APP_BUILD_NUMBER}")


def generate_icon():
    """Generate the main app icon and tray-state icon variants if stale.

    Both this script and the renderer are inputs: the styles and geometry live
    in tools/icon_render.py, so an edit there has to invalidate the .ico files
    just as an edit here does.
    """
    sources = [os.path.join(SCRIPT_DIR, "build.py"), icon_render.__file__]
    for variant, path, sizes in ICON_OUTPUTS:
        if build_state.any_newer(sources, path):
            icon_render.write_ico(path, variant, sizes)


MANIFEST_PATH = os.path.join(SCRIPT_DIR, "greencurve.exe.manifest")


def generate_resource_script():
    """Generate the deterministic Windows resource script and manifest if missing or stale."""
    rc_content = build_state.build_rc_content(APP_VERSION, APP_BUILD_NUMBER)
    manifest_content = build_state.build_manifest_content(APP_VERSION, APP_BUILD_NUMBER)
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
    if not build_state.any_newer(sources, ICON_RES):
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
    """Stage a pinned toolchain archive into the repository root.

    The vendored ``compilers/`` copy always wins over the network, and under
    GREENCURVE_TOOLCHAIN_LOCAL_ONLY there is no network path at all.  See
    tools/toolchain.py.
    """
    return toolchain.resolve_archive(
        source_label, archive_name, local_dir, url, sha256,
        os.path.join(SCRIPT_DIR, archive_name))


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


def _verify_cached_tool_binary(binary_path, label, trusted_sha256,
                               mismatch_expected=False):
    """Verify a cached tool binary against a digest pinned in this script.

    Adjacent ``.sha256`` files are not trusted: an attacker who can replace the
    binary can replace that sentinel too.  The sentinel is kept only as a
    post-extraction marker; the authority for cache reuse is the pinned digest.

    ``mismatch_expected`` is for the self-test that drives the tamper path on
    purpose (tools/security_gates.py).  A rejection is the assertion there, not
    a failure, and printing it as ERROR on an otherwise green run trains
    everyone -- and every CI log scanner -- to ignore the one line that means a
    real toolchain was swapped underneath us.
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
        if mismatch_expected:
            print(f"  {label}: tampered binary rejected as expected")
            return False
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
    toolchain.extract_archive(archive_path, ZIG_DIR,
                              is_zip=_ZIG_ARCHIVE_EXT == ".zip")
    os.remove(archive_path)

    if not os.path.exists(ZIG_EXE):
        print(f"ERROR: {_ZIG_EXE_NAME} not found after extraction")
        sys.exit(1)

    print(f"Zig installed at {ZIG_EXE}")
    if ZIG_EXE_SHA256 and not _verify_cached_tool_binary(ZIG_EXE, "zig", ZIG_EXE_SHA256):
        print("ERROR: Extracted Zig binary failed pinned executable digest verification")
        sys.exit(1)
    _write_integrity_sentinel(ZIG_EXE, ZIG_EXE_SHA256)


def _llvm_mingw_manifest():
    return toolchain.load_manifest(COMPILERS_DIR, "llvm-mingw", LLVM_MINGW_VERSION)


def download_llvm_mingw():
    """Download (or copy from local compilers/) and extract llvm-mingw toolchain."""
    # The driver digest alone is not enough to accept an existing tree: on Linux
    # it resolves to a wrapper script that never changes between releases, and a
    # restored CI cache is far easier to tamper with than a pinned archive.  The
    # manifest covers every binary in bin/, so a swapped linker or resource
    # compiler is caught here instead of silently shaping a release artifact.
    if (os.path.exists(LLVM_MINGW_CLANG)
            and _verify_cached_tool_binary(LLVM_MINGW_CLANG, "llvm-mingw", LLVM_MINGW_CLANG_SHA256)
            and toolchain.verify_tree("llvm-mingw", LLVM_MINGW_DIR, _llvm_mingw_manifest())):
        print(f"llvm-mingw already present at {LLVM_MINGW_CLANG}")
        return

    llvm_local_dir = os.path.join(COMPILERS_DIR, f"llvm-mingw-{LLVM_MINGW_VERSION}")
    archive_path = _resolve_archive(
        "llvm-mingw", LLVM_MINGW_ARCHIVE_NAME, llvm_local_dir,
        LLVM_MINGW_URL, LLVM_MINGW_SHA256)

    print("Extracting llvm-mingw...")
    shutil.rmtree(LLVM_MINGW_DIR, ignore_errors=True)
    toolchain.extract_archive(archive_path, LLVM_MINGW_DIR,
                              is_zip=LLVM_MINGW_ARCHIVE_EXT == ".zip",
                              materialize_links=True)
    os.remove(archive_path)

    if not os.path.exists(LLVM_MINGW_CLANG):
        print(f"ERROR: {os.path.basename(LLVM_MINGW_CLANG)} not found after extraction")
        sys.exit(1)

    print(f"llvm-mingw installed at {LLVM_MINGW_DIR}")
    if not _verify_cached_tool_binary(LLVM_MINGW_CLANG, "llvm-mingw", LLVM_MINGW_CLANG_SHA256):
        print("ERROR: Extracted llvm-mingw binary failed pinned executable digest verification")
        sys.exit(1)
    if not toolchain.verify_tree("llvm-mingw", LLVM_MINGW_DIR, _llvm_mingw_manifest()):
        print("ERROR: Extracted llvm-mingw failed pinned manifest verification")
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


def _object_debug_flags(pdb_path):
    """CodeView/prefix-map flags for object-first Windows x64 compiles."""
    if not pdb_path:
        return []
    return ["-gcodeview",
            f"-ffile-prefix-map={SCRIPT_DIR}=.",
            f"-fdebug-prefix-map={SCRIPT_DIR}=.",
            "-fdebug-compilation-dir=."]


def _windows_link_debug_flags(pdb_path, pdb_name):
    """Debug flags for the object-first Windows x64 link (emits the PDB)."""
    if not pdb_path:
        return []
    return [*_object_debug_flags(pdb_path), f"-Wl,--pdb={pdb_name}"]


def _compile_windows_x64_objects(object_dir, pdb_path, service=False, jobs=1, limiter=None):
    """Compile the Windows x64 TUs to LTO bitcode objects in parallel."""
    compile_flags = build_scheduler.compile_only_flags(
        [*COMMON_FLAGS, *WINDOWS_FLAGS], keep_lto=True)
    compile_flags.extend(_object_debug_flags(pdb_path))
    if service:
        compile_flags.append("-DGREEN_CURVE_SERVICE_BINARY=1")
    compiles = []
    objects = []
    for index, source in enumerate(WINDOWS_SOURCE_FILES):
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(object_dir, f"{index:02d}-{stem}.o")
        compiles.append((source,
                         [LLVM_MINGW_CLANG, *compile_flags, "-c", source, "-o", obj],
                         SCRIPT_DIR))
        objects.append(obj)
    build_scheduler.compile_objects(compiles, _run_compiler, jobs, limiter)
    return objects


def _link_windows_x64(temp_output, objects, pdb_path, service=False):
    """Link Windows x64 bitcode objects with the release flag set."""
    cmd = [LLVM_MINGW_CLANG, *COMMON_FLAGS, *WINDOWS_FLAGS,
           *_windows_link_debug_flags(
               pdb_path, "greencurve-service.pdb" if service else "greencurve.pdb"),
           "-o", temp_output, *objects, ICON_RES,
           *(WINDOWS_SERVICE_LINK_LIBS if service else WINDOWS_LINK_LIBS)]
    return _run_compiler(cmd, allow_cfg_collision=True)


def _linux_object_compile_flags(arch):
    return build_scheduler.compile_only_flags(
        [*COMMON_FLAGS, *linux_flags_for_arch(arch)], keep_lto=(arch == "x64"))


def _compile_linux_objects(object_dir, arch="x64", jobs=1, limiter=None):
    """Compile the Linux TUs to LTO bitcode objects in parallel."""
    compile_flags = _linux_object_compile_flags(arch)
    compiles = []
    objects = []
    for index, source in enumerate(LINUX_SOURCE_FILES):
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(object_dir, f"{index:02d}-{stem}.o")
        compiles.append((source,
                         [ZIG_EXE, "c++", *compile_flags, "-c", source, "-o", obj],
                         SCRIPT_DIR))
        objects.append(obj)
    build_scheduler.compile_objects(compiles, _run_compiler, jobs, limiter)
    return objects


def _link_linux_x64(temp_output, objects):
    """Link the Linux x64 objects with the release flag set."""
    cmd = [ZIG_EXE, "c++", *COMMON_FLAGS, *linux_flags_for_arch("x64"),
           "-o", temp_output, *objects]
    return _run_compiler(cmd)


def _compile_arm64_objects(sources, object_dir, target, extra_flags=None,
                           jobs=1, limiter=None):
    """Compile ARM64 translation units independently so branch protection is
    materialized before link. LTO drops BTI/PAC/AUT with the pinned Zig.
    """
    os.makedirs(object_dir, exist_ok=True)
    compile_flags = _compile_only_flags([*COMMON_FLAGS, *(extra_flags or [])])
    compiles = []
    objects = []
    for index, source in enumerate(sources):
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(object_dir, f"{index:02d}-{stem}.o")
        cmd = [ZIG_EXE, "c++", *compile_flags, "-target", target,
               "-mbranch-protection=standard", "-fno-lto", "-O2", "-c", source, "-o", obj]
        compiles.append((source, cmd, object_dir))
        objects.append(obj)
    build_scheduler.compile_objects(compiles, _run_compiler, jobs, limiter)
    return objects


def _link_arm64_windows(temp_output, sources, link_libs, symbol_path, service=False,
                        jobs=1, limiter=None):
    work = prepare_work_subdir("arm64-windows-service" if service else "arm64-windows-gui")
    try:
        definitions = ["-DGREEN_CURVE_SERVICE_BINARY=1"] if service else []
        objects = _compile_arm64_objects(
            sources, os.path.join(work, "obj"), "aarch64-windows-gnu",
            [*definitions, "-g", "-gdwarf-4", f"-ffile-prefix-map={SCRIPT_DIR}=.",
             f"-fdebug-prefix-map={SCRIPT_DIR}=.", "-fdebug-compilation-dir=.",
             "-ftrivial-auto-var-init=pattern",
             "-fno-delete-null-pointer-checks"],
            jobs=jobs, limiter=limiter)
        scratch_output = os.path.join(work, "greencurve-service.exe" if service else "greencurve.exe")
        scratch_symbols = scratch_output + ".debug"
        cmd = [ZIG_EXE, "c++", "-target", "aarch64-windows-gnu",
               "-mbranch-protection=standard", "-fno-lto", "-static",
               "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
               "-o", scratch_output, *objects, ICON_RES, *link_libs]
        with (limiter.slot() if limiter is not None else nullcontext()):
            if _run_compiler(cmd, cwd=work) != 0:
                raise RuntimeError("ARM64 Windows link failed")
        if subprocess.run([LLVM_MINGW_OBJCOPY, "--only-keep-debug", scratch_output, scratch_symbols],
                          cwd=work).returncode != 0:
            raise RuntimeError("ARM64 Windows private-symbol extraction failed")
        if subprocess.run([LLVM_MINGW_STRIP, "--strip-all", scratch_output], cwd=work).returncode != 0:
            raise RuntimeError("ARM64 Windows release strip failed")
        os.replace(scratch_output, temp_output)
        os.makedirs(os.path.dirname(symbol_path), exist_ok=True)
        os.replace(scratch_symbols, symbol_path)
    finally:
        cleanup_work_subdir(work)


def _link_arm64_linux(temp_output, sources, jobs=1, limiter=None):
    work = prepare_work_subdir("arm64-linux")
    try:
        objects = _compile_arm64_objects(
            sources, os.path.join(work, "obj"), LINUX_ARM64_TRIPLE,
            ["-fPIE", "-fstack-protector-strong", "-fexceptions", "-frtti",
             # This object-first path bypasses linux_flags_for_arch(), so the
             # flag has to be repeated here or arm64 silently loses it.
             "-ftrivial-auto-var-init=pattern",
             # Debug info + prefix maps, same as the x64 path.  Stripped back out
             # of the shipped binary by extract_linux_symbols().
             "-g", "-gdwarf-4", f"-ffile-prefix-map={SCRIPT_DIR}=.",
             f"-fdebug-prefix-map={SCRIPT_DIR}=.", "-fdebug-compilation-dir=."],
            jobs=jobs, limiter=limiter)
        cmd = [ZIG_EXE, "c++", "-target", LINUX_ARM64_TRIPLE,
               "-mbranch-protection=standard", "-fno-lto", "-O2", "-pie",
               "-Wl,-z,relro,-z,now", "-Wl,-z,noexecstack",
               "-Wl,--build-id=sha1",
               "-o", temp_output, *objects, "-ldl", "-lpthread"]
        with (limiter.slot() if limiter is not None else nullcontext()):
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
    major, minor, patch, build = build_state.parse_version_parts(
        APP_VERSION, APP_BUILD_NUMBER)
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


# x86 `endbr64`.  Shared with tools/security_gates.py, which does the deeper
# symbol-attributed analysis; this file only needs the strip-proof byte count.
_ENDBR64_BYTES = b"\xf3\x0f\x1e\xfa"


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
    elif os_name != "windows":
        # x86 CET forward edge, the analogue of the arm64 check above and the
        # tripwire for -fcf-protection=full silently ceasing to apply.  Counted
        # as instruction bytes so it survives the release -s strip.  This shipped
        # at zero until 2026-07-28, when the flag was Windows-only.
        endbr = data.count(_ENDBR64_BYTES)
        if endbr == 0:
            raise RuntimeError(
                "x86 CET instrumentation missing (no endbr64); "
                "-fcf-protection=full is not reaching the Linux build")
        print(f"  x86 CET forward edge: endbr64={endbr}")
    print(f"  Verified {os_name}-{arch}: architecture, version, hardening, sections, private paths")


def verify_windows_private_symbols(pdb_path, arch):
    """Require readable private postmortem symbols for every Windows artifact."""
    if not os.path.isfile(pdb_path) or os.path.getsize(pdb_path) < 4096:
        raise RuntimeError(f"matching Windows {arch} symbols are missing or empty: {pdb_path}")
    if arch == "arm64":
        result = subprocess.run([LLVM_MINGW_READOBJ, "--sections", pdb_path],
                                text=True, capture_output=True)
        if result.returncode != 0 or ".debug_info" not in result.stdout or \
                ".debug_line" not in result.stdout:
            raise RuntimeError(f"private ARM64 debug file failed structural verification: {pdb_path}")
        print(f"  Verified private symbols: {pdb_path} ({os.path.getsize(pdb_path):,} bytes)")
        return
    if not os.path.isfile(LLVM_MINGW_PDBUTIL):
        raise RuntimeError("llvm-pdbutil is missing; cannot verify private symbols")
    result = subprocess.run([LLVM_MINGW_PDBUTIL, "dump", "-summary", pdb_path],
                            text=True, capture_output=True)
    if result.returncode != 0 or "GUID:" not in result.stdout or \
            "Has Debug Info: true" not in result.stdout:
        raise RuntimeError(f"private PDB failed structural verification: {pdb_path}")
    print(f"  Verified private symbols: {pdb_path} ({os.path.getsize(pdb_path):,} bytes)")


def compile_windows_binary(output_path=WINDOWS_OUTPUT_EXE, temp_output=WINDOWS_TEMP_OUTPUT_EXE, backup_path=WINDOWS_BACKUP_EXE, finalize=True, arch="x64", jobs=1, limiter=None):
    """Compile the Windows GUI executable using Zig's bundled clang."""
    missing_sources = [path for path in WINDOWS_SOURCE_FILES if not os.path.exists(path)]
    if missing_sources:
        print("ERROR: Missing source files:")
        for path in missing_sources:
            print(f"  {path}")
        sys.exit(1)

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
    if arch == "arm64":
        print("  Mode: object-first Zig ARM64, LTO disabled")
    elif jobs > 1:
        print(f"  Mode: object-first clang x64, LTO enabled, up to {jobs} parallel jobs")
    else:
        print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_windows(temp_output, WINDOWS_SOURCE_FILES, WINDOWS_LINK_LIBS,
                                pdb_path, jobs=jobs, limiter=limiter)
            returncode = 0
        elif jobs > 1:
            work = prepare_work_subdir("obj-windows-x64-gui")
            try:
                objects = _compile_windows_x64_objects(work, pdb_path,
                                                       jobs=jobs, limiter=limiter)
                with (limiter.slot() if limiter is not None else nullcontext()):
                    returncode = _link_windows_x64(temp_output, objects, pdb_path)
            finally:
                cleanup_work_subdir(work)
            if returncode == 0:
                os.replace(link_pdb_path, pdb_path)
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


def compile_windows_service_binary(output_path=WINDOWS_SERVICE_OUTPUT_EXE, temp_output=WINDOWS_SERVICE_TEMP_OUTPUT_EXE, backup_path=WINDOWS_SERVICE_BACKUP_EXE, finalize=True, arch="x64", jobs=1, limiter=None):
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
    if arch == "arm64":
        print("  Mode: object-first Zig ARM64, LTO disabled")
    elif jobs > 1:
        print(f"  Mode: object-first clang x64, LTO enabled, up to {jobs} parallel jobs")
    else:
        print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_windows(temp_output, WINDOWS_SOURCE_FILES,
                                WINDOWS_SERVICE_LINK_LIBS, pdb_path, service=True,
                                jobs=jobs, limiter=limiter)
            returncode = 0
        elif jobs > 1:
            work = prepare_work_subdir("obj-windows-x64-service")
            try:
                objects = _compile_windows_x64_objects(work, pdb_path, service=True,
                                                       jobs=jobs, limiter=limiter)
                with (limiter.slot() if limiter is not None else nullcontext()):
                    returncode = _link_windows_x64(temp_output, objects, pdb_path,
                                                   service=True)
            finally:
                cleanup_work_subdir(work)
            if returncode == 0:
                os.replace(link_pdb_path, pdb_path)
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


def compile_linux_binary(output_path=LINUX_OUTPUT_BIN, temp_output=LINUX_TEMP_OUTPUT_BIN, backup_path=LINUX_BACKUP_BIN, finalize=True, arch="x64", jobs=1, limiter=None):
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
    if arch == "arm64":
        print("  Mode: object-first Zig ARM64, LTO disabled")
    elif jobs > 1:
        print(f"  Mode: object-first Zig x64, LTO enabled, up to {jobs} parallel jobs")
    else:
        print(f"  Command: {' '.join(cmd)}")

    compile_started_at = time.time()
    try:
        if arch == "arm64":
            _link_arm64_linux(temp_output, LINUX_SOURCE_FILES, jobs=jobs, limiter=limiter)
            returncode = 0
        elif jobs > 1:
            work = prepare_work_subdir("obj-linux-x64")
            try:
                objects = _compile_linux_objects(work, "x64", jobs=jobs, limiter=limiter)
                with (limiter.slot() if limiter is not None else nullcontext()):
                    returncode = _link_linux_x64(temp_output, objects)
            finally:
                cleanup_work_subdir(work)
        else:
            returncode = _run_compiler(cmd)
        if returncode == 0:
            # Split symbols out and strip BEFORE verification, so every check —
            # including the private-workspace-path scan, which stays strict —
            # runs against the exact bytes that ship.
            crash_artifacts.extract_linux_symbols(_gate_ctx(), temp_output, arch)
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
# Linux-only: the daemon setup wrapper ships next to the binary it drives.
LINUX_SETUP_SCRIPT_PATH = os.path.join(SCRIPT_DIR, "tools", "greencurve-setup.sh")


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


def package_release_archive(os_name, arch, binaries, seven=None):
    """Stage and archive an exact per-platform allowlist, then read it back.

    main() resolves `seven` up front so a missing 7-Zip becomes one skip for the
    whole run; reaching this function still means packaging was requested.  Only
    the Windows container needs it -- the Linux tarball is written by the
    standard library, which is also the only way a Windows host can record the
    Unix modes the daemon and its setup script need."""
    payload = target_payload_dir(os_name, arch)
    purge_runtime_artifacts(payload)
    binary_names = {os.path.basename(path) for path in binaries}
    expected_names = expected_release_names(os_name)
    staged_extras = {"README.md", "LICENSE"}
    if os_name == "linux":
        staged_extras.add("greencurve-setup.sh")
    if binary_names != expected_names - staged_extras:
        raise RuntimeError(f"binary allowlist mismatch for {os_name}: {sorted(binary_names)}")
    for binary in binaries:
        if not os.path.exists(binary):
            raise RuntimeError(f"missing build output {binary}")
        actual_arch = detect_binary_arch(binary)
        if actual_arch != arch:
            raise RuntimeError(f"architecture mismatch: {binary} is {actual_arch}, expected {arch}")
    validate_payload_file_names(payload, binary_names)
    required_files = [README_MD_PATH, LICENSE_PATH]
    if os_name == "linux":
        required_files.append(LINUX_SETUP_SCRIPT_PATH)
    for required in required_files:
        if not os.path.isfile(required):
            raise RuntimeError(f"required release file missing: {required}")

    if os_name == "windows":
        seven = seven or find_seven_zip()
        if not seven:
            raise RuntimeError("7-Zip is required to produce verified Windows release archives")
    archive = os.path.join(
        SCRIPT_DIR, f"greencurve-{APP_VERSION}-{os_name}-{arch}{release_archive_extension(os_name)}")
    # Also clears a same-target archive left by an earlier container format, so
    # a stale and now-known-broken .7z cannot ship beside the current tarball.
    for stale in release_archive_paths(SCRIPT_DIR, APP_VERSION, os_name, arch):
        if os.path.exists(stale):
            os.remove(stale)
    work = prepare_work_subdir(f"package-{os_name}-{arch}")
    root = release_archive_root(os_name)
    try:
        staging = os.path.join(work, root)
        os.makedirs(staging, exist_ok=True)
        for binary in binaries:
            shutil.copy2(binary, os.path.join(staging, os.path.basename(binary)))
        for extra in required_files:
            # Linux text is rewritten to LF rather than copied: this working
            # tree is CRLF on a Windows host, and a CRLF greencurve-setup.sh is
            # an unrunnable script (`#!/usr/bin/env bash\r`).
            stage_release_file(extra, os.path.join(staging, os.path.basename(extra)),
                               normalize=(os_name == "linux"))
        validate_payload_file_names(staging, expected_names)
        if os_name == "linux":
            write_linux_tarball(archive, staging, root, expected_names)
            verify_linux_tarball(archive, expected_names, root)
        else:
            result = subprocess.run(
                [seven, "a", "-t7z", "-mx=9", "-bso0", "-bsp0", archive, root],
                cwd=work,
            )
            if result.returncode != 0 or not os.path.exists(archive):
                raise RuntimeError(f"7-Zip archiving failed for {os_name}-{arch}")
            verify_seven_zip_manifest(seven, archive, expected_names, root)
            # The setup file ships the same verified manifest, plus the
            # uninstaller, so an archive and an installed copy are the same
            # bits.  Staged here rather than from dist/ because this folder has
            # already passed the allowlist and architecture checks above.
            installer_build.build_setup_executable(_gate_ctx(), arch, staging, expected_names)
    finally:
        cleanup_work_subdir(work)
    size = os.path.getsize(archive)
    print(f"Archived {os.path.basename(archive)} ({size:,} bytes / {size / 1024:.1f} KB)")
    hash_path = archive + ".sha256"
    with open(hash_path, "w") as f:
        f.write(f"{_sha256_file(archive)}  {os.path.basename(archive)}\n")


# The hardware-free driver inspectors (Linux aarch64 and Windows on Arm) live in
# tools/driver_inspect.py; see the import block at the top of this file.


def requested_arches(arch):
    if arch == "all":
        return ["x64", "arm64"]
    if arch in ("x64", "arm64"):
        return [arch]
    raise ValueError(f"unsupported architecture: {arch}")


def resolve_targets(requested):
    """Return the requested OS matrix on every supported build host."""
    return ["windows", "linux"] if requested == "all" else [requested]


def run_check_builds(target, arch="all", generate_lsp=True, jobs=1, limiter=None):
    """Build selected targets into a temporary directory without replacing release outputs."""
    if generate_lsp:
        generate_lsp_files()
    if target in ("windows", "all"):
        # Shared resources: generate once before any parallel worker touches them.
        generate_icon()
        compile_resources()
    tmp = prepare_work_subdir("check")
    try:
        specs = []
        for selected_arch in requested_arches(arch):
            arch_tmp = os.path.join(tmp, selected_arch)
            os.makedirs(arch_tmp, exist_ok=True)
            if target in ("windows", "all"):
                gui = os.path.join(arch_tmp, "greencurve.exe")
                svc = os.path.join(arch_tmp, "greencurve-service.exe")
                specs.append(lambda gui=gui, selected_arch=selected_arch:
                             compile_windows_binary(
                                 output_path=gui, temp_output=gui + ".new",
                                 backup_path="", arch=selected_arch, finalize=False,
                                 jobs=jobs, limiter=limiter))
                specs.append(lambda svc=svc, selected_arch=selected_arch:
                             compile_windows_service_binary(
                                 output_path=svc, temp_output=svc + ".new",
                                 backup_path="", arch=selected_arch, finalize=False,
                                 jobs=jobs, limiter=limiter))
            if target in ("linux", "all"):
                suffix = LINUX_ARM64_TRIPLE if selected_arch == "arm64" else LINUX_TARGET
                out = os.path.join(arch_tmp, f"greencurve-{suffix}")
                specs.append(lambda out=out, selected_arch=selected_arch:
                             compile_linux_binary(
                                 output_path=out, temp_output=out + ".new",
                                 backup_path="", arch=selected_arch, finalize=False,
                                 jobs=jobs, limiter=limiter))
        build_scheduler.run_parallel(specs, jobs)
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

    # The regression harness is a real translation unit now, so give it the
    # same LSP/clang-tidy coverage as production code. Match the current host:
    # a Linux clang-tidy cannot parse llvm-mingw-only flags such as -mguard=cf.
    harness_source = os.path.join(SCRIPT_DIR, "tests", "regression_main.cpp")
    if os.path.exists(harness_source):
        if sys.platform == "win32":
            harness_flags = [*windows_flags, f"-I{SOURCE_DIR}"]
        else:
            harness_linux_flags = [
                flag for flag in linux_flags
                if flag not in ("-fexceptions", "-frtti")
            ]
            harness_flags = [
                *harness_linux_flags, *zig_linux_analyzer_flags(),
                "-fno-exceptions", "-fno-rtti",
                f"-I{SOURCE_DIR}", "-include",
                os.path.join(SOURCE_DIR, "win32_compat.h"),
            ]
        entries.append({
            "directory": SCRIPT_DIR,
            "file": harness_source,
            "arguments": ["clang++", *harness_flags,
                          "-fsyntax-only", harness_source],
        })

    path = os.path.join(SCRIPT_DIR, "compile_commands.json")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(entries, handle, indent=2)
        handle.write("\n")
    print(f"Generated {path}")


def run_build_script_regression_tests():
    """Delegate; security_gates owns the build-script self-tests and gates."""
    return security_gates.run_build_script_regression_tests(_gate_ctx())


def _posix_test_compiler(extra_flags):
    """Delegate to security_gates, which owns sanitizer toolchain resolution."""
    return security_gates.posix_test_compiler(_gate_ctx(), extra_flags)


def run_regression_tests(extra_flags=None):
    """Run pure regression tests that do not touch GPU hardware."""
    run_build_script_regression_tests()
    harness_source = os.path.join(SCRIPT_DIR, "tests", "regression_main.cpp")
    if not os.path.exists(harness_source):
        print(f"Regression harness missing: {harness_source}")
        sys.exit(1)
    tmp = prepare_work_subdir("test")
    try:
        # The harness is a real .cpp under tests/ rather than a string literal
        # in this script, so it gets LSP, clang-format and clang-tidy coverage.
        harness_path = harness_source
        test_exe = os.path.join(tmp, "fan_curve_regression.exe" if sys.platform == "win32" else "fan_curve_regression")
        # llvm-mingw clang++ takes flags directly; zig needs the c++ subcommand,
        # and an ASan build needs a host clang instead of Zig entirely.
        cmd = ([LLVM_MINGW_CLANG] if sys.platform == "win32"
               else _posix_test_compiler(extra_flags))
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
            os.path.join(SOURCE_DIR, "config_text_utils.cpp"),
            os.path.join(SOURCE_DIR, "app_shared.cpp"),
            os.path.join(SOURCE_DIR, "vf_backends.cpp"),
        ])
        if sys.platform == "win32":
            # Win32-only implementations; the harness #ifdefs out the suites
            # that exercise them (config INI, DACLs, Task Scheduler XML).
            for win_only in ("config_utils.cpp", "service_acl.cpp", "platform_win32.cpp"):
                cmd.append(os.path.join(SOURCE_DIR, win_only))
        else:
            # win32_compat.h is force-included, never #included by the harness.
            cmd.append(os.path.join(SOURCE_DIR, "platform_posix.cpp"))
            cmd.extend(["-include", os.path.join(SOURCE_DIR, "win32_compat.h")])
        if extra_flags:
            cmd.extend(extra_flags)
        if sys.platform == "win32":
            cmd.extend(["-static", "-luser32", "-lgdi32", "-luuid", "-ladvapi32",
                        # bcrypt: the harness compiles the real CNG update
                        # verifier for the signer/verifier known-answer test.
                        "-lshell32", "-lbcrypt"])
        else:
            cmd.extend(["-lpthread", "-ldl"])
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
        # Fixtures that need real Linux kernel behaviour -- filesystem sockets
        # for the transport, fork/signal delivery and file creation for the
        # crash report -- so they run natively on Linux and are cross-compiled
        # everywhere else.  Compiling them on every host is the point: a break
        # in one would otherwise stay invisible until someone happened to test
        # on Linux.
        for stem, label in (("linux_transport_regression", "socket transport"),
                            ("linux_crash_report_regression", "crash report")):
            fixture_source = os.path.join(SCRIPT_DIR, "tests", f"{stem}.cpp")
            if sys.platform.startswith("linux"):
                fixture_exe = os.path.join(tmp, stem)
                fixture_cmd = [
                    *_posix_test_compiler(extra_flags), "-std=c++17", "-DNDEBUG",
                    f'-DAPP_VERSION="{APP_VERSION}"',
                    f"-DAPP_BUILD_NUMBER={APP_BUILD_NUMBER}",
                    "-fno-exceptions", "-fno-rtti",
                    f"-I{SOURCE_DIR}",
                    "-o", fixture_exe,
                    fixture_source,
                ]
                if extra_flags:
                    fixture_cmd.extend(extra_flags)
                print(f"Compiling Linux {label} regression tests")
                result = subprocess.run(fixture_cmd, cwd=SCRIPT_DIR)
                if result.returncode != 0:
                    print(f"Linux {label} test compilation FAILED")
                    sys.exit(result.returncode)
                print(f"Running Linux {label} regression tests")
                result = subprocess.run([fixture_exe], cwd=SCRIPT_DIR,
                                        env=test_env)
                if result.returncode != 0:
                    print(f"Linux {label} regression FAILED ({result.returncode})")
                    sys.exit(result.returncode)
            else:
                fixture_cmd = [
                    ZIG_EXE, "c++", "-std=c++17", "-DNDEBUG",
                    f'-DAPP_VERSION="{APP_VERSION}"',
                    f"-DAPP_BUILD_NUMBER={APP_BUILD_NUMBER}",
                    "-fno-exceptions", "-fno-rtti",
                    "-target", "x86_64-linux-gnu",
                    "-Wall", "-Wextra", "-Wno-unused-function",
                    "-Wno-unused-parameter", "-Werror",
                    f"-I{SOURCE_DIR}",
                    "-c", fixture_source,
                    "-o", os.path.join(tmp, f"{stem}.o"),
                ]
                print(f"Cross-compiling Linux {label} regression tests")
                result = subprocess.run(fixture_cmd, cwd=SCRIPT_DIR)
                if result.returncode != 0:
                    print(f"Linux {label} test cross-compilation FAILED")
                    sys.exit(result.returncode)
        # Native named-pipe incident regression; details live in security_gates.
        security_gates.run_windows_pipe_fixture(_gate_ctx(), tmp, extra_flags)
        run_source_regression_checks()
        print("Regression tests passed")
    finally:
        cleanup_work_subdir(tmp)


# security_gates reads SCRIPT_DIR, SOURCE_DIR, LLVM_MINGW_DIR,
# LLVM_MINGW_CLANG, APP_VERSION, APP_BUILD_NUMBER, prepare_work_subdir and
# cleanup_work_subdir off this module.  Passing the live module rather than a
# snapshot keeps APP_BUILD_NUMBER current after configure_build_number() runs.
def _gate_ctx():
    return sys.modules[__name__]


def run_fuzz_targets(runs=None, target_filter=None):
    return security_gates.run_fuzz_targets(_gate_ctx(), runs=runs,
                                           target_filter=target_filter)


def check_cet_instrumentation(binary_path=None):
    return security_gates.check_cet_instrumentation(_gate_ctx(), binary_path)


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


def _tracked_repository_files():
    """Tracked paths, or None when git is unavailable (tarball/export builds).

    The check that uses this must not fail a legitimate build just because git
    is missing, so an unavailable index is treated as "cannot assess".
    """
    try:
        result = subprocess.run(["git", "ls-files"], cwd=SCRIPT_DIR,
                                text=True, capture_output=True)
    except (OSError, ValueError):
        return None
    if result.returncode != 0:
        return None
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def require_text_in_surface(paths, needle, label):
    """Assert `needle` appears in at least one shard of a logical surface.

    Splitting an oversized module used to break every guard that named the
    original file, which discouraged the very splits the size ratchet asks for.
    A surface is the aggregator plus the shards it `#include`s, so a guard keeps
    holding when code moves between them.
    """
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            if needle in handle.read():
                return
    print(f"Regression source check FAILED: {label}")
    sys.exit(1)


def forbid_text_in_surface(paths, needle, label):
    """Assert `needle` appears in no shard of a logical surface."""
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            if needle in handle.read():
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
def check_fuzz_harness_in_sync():
    """Delegate to tools/security_gates.py, which owns the fuzz target table."""
    security_gates.check_fuzz_harness_in_sync(_gate_ctx(), require_text, forbid_text)


def run_source_regression_checks():
    build_state.enforce_source_size_ratchet(SCRIPT_DIR, SOURCE_DIR)
    check_fuzz_harness_in_sync()
    check_packaging_skip_warning()
    security_gates.check_no_developer_profile_paths(
        _gate_ctx(), _tracked_repository_files())
    security_gates.check_no_signing_key_material(
        _gate_ctx(), _tracked_repository_files())
    require_app_version_fallback_in_sync()
    main_cpp = os.path.join(SOURCE_DIR, "main.cpp")
    entry_cpp = os.path.join(SOURCE_DIR, "entry.cpp")
    diagnostics_cpp = os.path.join(SOURCE_DIR, "main_diagnostics.cpp")
    crash_artifacts_cpp = os.path.join(SOURCE_DIR, "main_crash_artifacts.cpp")
    secure_write_cpp = os.path.join(SOURCE_DIR, "main_secure_write.cpp")
    service_ipc_aggregate_cpp = os.path.join(SOURCE_DIR, "main_service_ipc.cpp")
    service_connection_cpp = os.path.join(SOURCE_DIR, "main_service_connection.cpp")
    service_client_commands_cpp = os.path.join(SOURCE_DIR, "main_service_client_commands.cpp")
    service_admin_client_cpp = os.path.join(SOURCE_DIR, "main_service_admin_client.cpp")
    service_machine_config_cpp = os.path.join(SOURCE_DIR, "main_service_machine_config.cpp")
    service_server_aggregate_cpp = os.path.join(SOURCE_DIR, "main_service_server.cpp")
    service_request_policy_cpp = os.path.join(SOURCE_DIR, "main_service_request_policy.cpp")
    service_pipe_cpp = os.path.join(SOURCE_DIR, "main_service_pipe.cpp")
    service_pipe_switch_cpp = os.path.join(SOURCE_DIR, "main_service_pipe_switch.cpp")
    service_pipe_listener_cpp = os.path.join(SOURCE_DIR, "main_service_pipe_listener.cpp")
    service_host_cpp = os.path.join(SOURCE_DIR, "main_service_host.cpp")
    config_utils_cpp = os.path.join(SOURCE_DIR, "config_utils.cpp")
    fan_curve_cpp = os.path.join(SOURCE_DIR, "fan_curve.cpp")
    config_profiles_ui_cpp = os.path.join(SOURCE_DIR, "config_profiles_ui.cpp")
    config_profiles_gui_state_cpp = os.path.join(
        SOURCE_DIR, "config_profiles_gui_state.cpp")
    desired_settings_helpers_cpp = os.path.join(SOURCE_DIR, "desired_settings_helpers.cpp")
    config_profile_repair_cpp = os.path.join(SOURCE_DIR, "config_profile_repair.cpp")
    gpu_backend_apply_cpp = os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp")
    main_gpu_state_cpp = os.path.join(SOURCE_DIR, "main_gpu_state.cpp")
    main_data_paths_cpp = os.path.join(SOURCE_DIR, "main_data_paths.cpp")
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
    cli_options_cpp = os.path.join(SOURCE_DIR, "main_cli_options.cpp")
    gpu_backend_cpp = os.path.join(SOURCE_DIR, "gpu_backend.cpp")
    gpu_selection_config_cpp = os.path.join(SOURCE_DIR, "gpu_selection_config.cpp")
    main_runtime_control_cpp = os.path.join(SOURCE_DIR, "main_runtime_control.cpp")
    main_runtime_capture_cpp = os.path.join(SOURCE_DIR, "main_runtime_capture.cpp")
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
    platform_h = os.path.join(SOURCE_DIR, "platform.h")
    platform_win32_cpp = os.path.join(SOURCE_DIR, "platform_win32.cpp")
    platform_posix_cpp = os.path.join(SOURCE_DIR, "platform_posix.cpp")
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
        for _cpp in (service_request_policy_cpp,
                     os.path.join(SOURCE_DIR, "main_service_pipe_primitives.h"),
                     os.path.join(SOURCE_DIR, "main_service_pipe_file_commands.cpp"),
                     service_pipe_cpp,
                     os.path.join(SOURCE_DIR, "service_ipc_throttle_policy.h"),
                     os.path.join(SOURCE_DIR, "service_pipe_prefix_read.h"),
                     service_pipe_switch_cpp,
                     service_pipe_listener_cpp,
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
    # ui_main_context_menus.cpp is the right-click-menu half of
    # ui_main_window.cpp, split out only to stay inside the source-size ratchet;
    # the guards address the two as one logical surface.
    ui_main_window_cpp = build_state.concatenated_gate_surface(
        BUILD_WORK_DIR, SOURCE_DIR, "_ui_main_window_surface.cpp",
        ("ui_main_window.cpp", "ui_main_context_menus.cpp"))
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
        # service_protocol_validation.h is the envelope/response half of
        # service_protocol.h, split out only to stay inside the source-size
        # ratchet; the guards address the two as one logical surface.
        for _h in (os.path.join(SOURCE_DIR, "app_shared.h"), gpu_core_h,
                   service_protocol_h,
                   os.path.join(SOURCE_DIR, "service_protocol_validation.h")):
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
    require_text(shared_h, "SERVICE_PROTOCOL_VERSION = 20",
                 "service protocol publishes outcome severity, update state and XBAR")
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
    # Debug logs are size-capped: every append passes the rotation check, and a
    # rotated file opens with an explanatory marker (2026-08 unbounded-growth fix).
    require_text(diagnostics_cpp, '#include "debug_log_rotation_policy.h"',
                 "debug log size-cap policy is compiled into the diagnostics shard")
    require_order_in_operation(diagnostics_cpp,
        "static void debug_log(const char* fmt, ...)",
        "gc_debug_log_rotation::should_rotate(",
        "WriteFile(g_debugLogFile",
        "debug log lines append only after the size-cap rotation check")
    require_text(diagnostics_cpp, "gc_debug_log_rotation::marker_line",
                 "a truncated debug log opens with an explanatory marker")
    crash_artifacts.check_windows_crash_artifacts(
        _gate_ctx(), require_text, forbid_text, require_order, require_text_count)
    crash_artifacts.check_linux_symbols(_gate_ctx(), require_text)
    require_text(secure_write_cpp, "write_all_to_handle", "file writes use size_t-safe chunked write helper")
    require_text(main_cpp, "SERVICE_PIPE_SERVER_IO_TIMEOUT_MS", "service pipe server I/O timeout exists")
    require_text(service_server_cpp, "CancelIoEx(pipe, &ov)", "stalled pipe operations are cancellable")
    require_text(service_server_cpp, "response.serviceBuildNumber", "service responses include build number")
    require_text(service_server_cpp, "restricted ACL creation returned no descriptor", "service pipe creation fails closed without ACL")
    require_text(service_server_cpp, "FATAL failed to create pipe listener pool", "service fails closed when the pipe listener pool cannot start")
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
    require_text(main_data_paths_cpp, "service_sid_string_from_token", "service user path cache resolves the caller SID")
    require_text(main_data_paths_cpp, "g_serviceUserPathsSid", "service user path cache keys by session id plus SID")
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
        "WaitForMultipleObjects(2, pipeReadyOrPrimary",
        "lifecycle worker is ready before pipe readiness is accepted")
    require_order_in_operation(service_host_cpp, service_main_anchor,
        "WaitForMultipleObjects(2, pipeReadyOrPrimary",
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
        "static void service_execute_checked_request(ServiceRequest* request,",
        "&g_serviceClientRequestsReady",
        '#include "main_service_pipe_switch.cpp"',
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
    require_order_in_operation(service_pipe_switch_cpp, "case SERVICE_CMD_APPLY:",
        "lock_service_runtime();",
        "service_explicit_supersede_automatic_work_locked(",
        "explicit Apply serializes lifecycle/helper supersession under the runtime lock")
    require_order_in_operation(service_pipe_switch_cpp, "case SERVICE_CMD_APPLY:",
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
    require_text_in_operation(service_pipe_switch_cpp, "case SERVICE_CMD_APPLY:",
        "if (explicitUserApply && proofRecorded)",
        "only explicit successful Apply enters the durable lockout/history acknowledgement branch")
    require_order_in_operation(service_pipe_switch_cpp, "case SERVICE_CMD_APPLY:",
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
    require_text(crash_artifacts_cpp, "crash_artifact_data_dir", "service crash artifacts route through a process-appropriate data directory")
    require_text(crash_artifacts_cpp, "resolve_service_machine_data_dir", "service crash artifacts use the machine service data directory")
    require_text(config_profile_repair_cpp, "savedOffsetMagnitude = savedOffset < 0 ? -(long long)savedOffset", "profile repair avoids abs(INT_MIN) overflow")
    require_text(service_ipc_cpp, "pl_append_quoted_arg_w", "elevated helper command lines use argv-compatible quoting")
    require_text(ui_main_window_cpp, "pl_append_quoted_arg_w", "GUI elevated helper command lines use argv-compatible quoting")
    require_text(build_script, "_verify_cached_tool_binary", "cached tool binaries are verified against trusted pinned digests")
    require_text(build_script, "LLVM_MINGW_CLANG_SHA256", "llvm-mingw executable digest is pinned")
    check_toolchain_pinning(build_script)
    require_text(service_ipc_cpp, "wait_for_helper_process_bounded", "elevated helper waits are bounded")
    require_text(config_profiles_gui_state_cpp, "repair needed", "broken installed service advertises repair state")
    require_text(ui_main_window_cpp, "Repair and restart the background service", "broken installed service click repairs instead of removing")
    forbid_text(config_profiles_ui_cpp, "maybe_load_selected_profile_to_gui_without_apply", "startup cannot restore saved selected-slot intent as live GPU state")
    require_text(logon_startup_cpp, "show_live_gpu_state_for_disabled_app_launch", "disabled app-start refreshes the editor from the service snapshot")
    require_text(logon_startup_cpp, "saved slot deliberately not loaded", "startup diagnostics distinguish saved profile intent from live state")
    require_text_in_operation(logon_startup_cpp,
        "static void maybe_load_app_launch_profile_to_gui()",
        "gui_service_model_ready(&g_app.guiServiceModel)",
        "startup live-state projection waits for an accepted READY envelope")
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
    xbar_gates.check_all(_gate_ctx(), require_text, forbid_text)
    require_text(main_gpu_state_cpp, "live_selective_gpu_offset_matches_requested_shape", "persisted selective GPU offset is verified against live VF shape")
    require_text(main_gpu_state_cpp, "runtime selective: ignoring persisted request", "stale persisted selective GPU offset is ignored")
    require_text(main_gpu_state_cpp, "non-selective request clears runtime state", "uniform GPU offsets clear runtime selective state")
    require_text(main_gpu_state_cpp, 'debug_log_on_change("current_applied_gpu_offset_mhz: not Blackwell', "stable non-Blackwell GPU offset diagnostic is change-gated")
    forbid_text(main_gpu_state_cpp, 'debug_log("current_applied_gpu_offset_mhz: not Blackwell', "stable non-Blackwell GPU offset diagnostic must not spam every poll")
    require_text(main_runtime_capture_cpp, 'debug_log_on_change("populate_global_controls: dirty=', "global control refresh diagnostics are change-gated")
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
    require_text(gpu_backend_apply_cpp, "service_apply_outcome_severity_for_lock_mode(", "hard NVML pins do not surface VF tail readback as a warning")
    require_text(gpu_backend_apply_cpp, "high offset warning summary", "large VF offset diagnostics are aggregated")
    require_text(gpu_backend_cpp, "update_tray_icon", "VF/GPU offset applies update tray icon from GUI-side apply path")
    require_text(gpu_backend_cpp, "parse_mhz_value_prefix", "nvidia-smi clock parsing is strict")
    require_text(os.path.join(SOURCE_DIR, "tray_presentation.cpp"), "if (g_app.freqOffsets[i] != 0) {", "live_state_has_custom_oc checks freqOffsets without vfBackend guard")
    require_text(runtime_nvml_cpp, "rollback_changed_fans", "manual multi-fan writes roll back partial failures")
    require_text(runtime_nvml_cpp, "nvml_select_device_for_selected_gpu", "NVML device is matched to selected GPU")
    require_text(secure_write_cpp, "write_text_file_atomic_service", "service file writes use hardened writer")
    require_text(ui_main_controls_cpp, "gpuSelectY = dp(10)", "GPU selector lives in the graph header gap")
    # Main-window Apply gates (origin, range hints, high-overclock confirm).
    ui_gates.check_all(_gate_ctx(), require_text, forbid_text)
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
    # Every other owner-draw checkbox guard, plus the right-anchored placement
    # rule, lives in tools/ui_gates.py.
    require_text(ui_main_layout_cpp, "SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE", "scrolling moves children and erases exposed pixels atomically")
    require_order_in_operation(ui_main_layout_cpp,
        "static bool main_layout_set_scroll(HWND hwnd, int x, int y)",
        "ScrollWindowEx(hwnd, oldX - x, oldY - y",
        "RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW",
        "scrolling synchronously redraws the complete settled frame")
    require_order_in_operation(os.path.join(SOURCE_DIR, "ui_main_control_lifecycle.cpp"),
        "static void rebuild_edit_controls()",
        "main_layout_grow_window_for_content(",
        "create_edit_controls(hwnd, g_app.hInst)",
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
    # GUI lock-checkbox rendering moved to tools/ui_gates.py
    # (check_lock_checkbox_render).  The remaining rules here cover the tri-state
    # gesture: only BN_CLICKED may advance it -- BS_OWNERDRAW emits BN_DBLCLK
    # automatically, while focus codes require BS_NOTIFY (which these controls do
    # not use) -- every notification is logged before the policy filters it, and
    # an armed gesture is consumed once and rejected if the lock model changes
    # between press and release (for example, a startup service snapshot).
    require_text(ui_main_window_cpp, "decide_lock_activation(", "lock tri-state commands use the executable activation policy")
    require_text(ui_main_window_cpp, "lock checkbox command: vi=%d notify=%u decision=", "all lock notifications and decisions are logged")
    require_text(ui_lock_checkbox_cpp, "activate_lock_checkbox_once", "lock tri-state transition is centralized")
    require_text(ui_lock_checkbox_cpp, "lock_checkbox_subclass_proc", "lock checkbox subclass exists")
    require_text(ui_lock_checkbox_cpp, "WM_LBUTTONDBLCLK", "lock checkbox double-click cannot advance the tri-state twice")
    require_text(ui_lock_checkbox_cpp, "paired double-click release suppressed", "double-click paired release cannot become an unarmed click")
    require_text(ui_main_controls_cpp, "lock checkbox subclass install FAILED", "subclass installation failure is diagnosed")
    require_text(ui_main_controls_cpp, "SetLastError(ERROR_SUCCESS);", "subclass failure logging does not report a stale Win32 error")
    require_text(cli_options_cpp, "parse_cli_point_arg_w(arg, &idx)", "CLI point parsing is strict")
    require_text(entry_cpp, "set_main_window_title", "window caption helper exists")
    require_text(entry_cpp, "SetWindowTextA", "window caption uses ANSI text write")
    require_text(entry_cpp, "RegisterClassExA", "main window uses ANSI class registration")
    require_text(entry_cpp, "CreateWindowExA", "main window uses ANSI creation path")
    # The usage text moved to main_cli_help.cpp when entry.cpp hit its ratchet.
    cli_help_cpp = os.path.join(SOURCE_DIR, "main_cli_help.cpp")
    require_text(cli_help_cpp, "--set-machine-logon-slot", "CLI supports setting machine default logon slot")
    require_text(cli_help_cpp, "--clear-machine-logon-slot", "CLI supports clearing machine default logon slot")
    require_text(cli_help_cpp, "--self-test", "the read-only pre-flight is documented in the usage text")
    require_text(config_profiles_ui_cpp, "update_share_all_users_check_state", "GUI updates the share-with-all-users checkbox state")
    require_text(config_profiles_ui_cpp, "refresh_machine_logon_slot_cache", "GUI refreshes machine logon slot cache")
    require_text(os.path.join(SOURCE_DIR, "single_instance_win32.cpp"),
                 "FindWindowA", "single-instance lookup uses ANSI class matching")
    require_text(build_script, "--check", "build check flag exists")
    require_text(build_script, "--test", "test flag exists")
    require_text(build_script, "compile_commands.json", "LSP database generation exists")
    require_text(gitignore, "*.7z", "Windows release archives are ignored")
    require_text(gitignore, "*.tar.xz", "Linux release archives are ignored")
    require_text(gitignore, "__pycache__/", "generated Python bytecode is ignored")

    # --- Static analysis and CI coverage (audit F-GATES)
    build_script_path = os.path.join(SCRIPT_DIR, "build.py")
    ci_workflow = os.path.join(SCRIPT_DIR, ".github", "workflows", "ci.yml")
    require_text(build_script_path, "--tidy",
                 "build.py exposes the clang-tidy entry point")
    # The harness lives in tests/ as real C++, not as a string literal in this
    # script.  Keep it there so it retains LSP/clang-format/clang-tidy coverage.
    harness_source_path = os.path.join(SCRIPT_DIR, "tests", "regression_main.cpp")
    require_text(harness_source_path, "int main(int argc, char** argv)",
                 "the pure regression harness is a real translation unit")
    # Assembled at runtime so this guard cannot match its own definition.
    forbid_text(build_script_path, "harness = " + "r" + "'''",
                "the regression harness must not move back into a build.py string")
    require_text(build_script_path, 'os.path.join(SCRIPT_DIR, "tests", "regression_main.cpp")',
                 "build.py compiles the extracted harness from tests/")
    # Every updater gate -- the signature-before-parse ordering, the consent
    # requirement, the TLS and staging rules -- lives in tools/update_gates.py.
    update_gates.check_all(_gate_ctx(), require_text, forbid_text,
                           require_order_in_operation, harness_source_path)
    # The ratchet's own guards live beside it, in tools/static_analysis.py.
    static_analysis.check_ratchet_wiring(_gate_ctx(), require_text)
    if os.path.exists(ci_workflow):
        # tests/linux_transport_regression.cpp needs real Linux filesystem
        # sockets, so the Linux job is the only place it can ever execute.
        require_text(ci_workflow, "python build.py --test",
                     "CI runs the regression suite")
        require_text(ci_workflow, "actions/cache",
                     "CI caches the pinned toolchains instead of redownloading them")
        require_text(ci_workflow, "python build.py --tidy",
                     "CI enforces the static-analysis ratchet")
        require_text(ci_workflow, "python build.py --fuzz",
                     "CI fuzzes untrusted-input boundaries")
        require_text(ci_workflow, "python build.py --target windows",
                     "CI builds verified Windows release packages")
        require_text(ci_workflow, "python build.py --target linux",
                     "CI builds verified Linux release packages")
        with open(ci_workflow, "r", encoding="utf-8", errors="replace") as handle:
            linux_section = handle.read().split("linux:", 1)[-1]
        if "python build.py --test" not in linux_section:
            print("Regression source check FAILED: CI Linux job must run the "
                  "regression suite; it is the only host that can execute the "
                  "native socket fixture")
            sys.exit(1)
    require_text(gitignore, "*.pyc", "compiled Python bytecode is ignored")
    # A tracked build.cpython-*.pyc previously embedded the developer's absolute
    # source path.  Generated artifacts must never be under version control.
    _tracked = _tracked_repository_files()
    if _tracked is not None:
        _generated = sorted(p for p in _tracked
                            if p.endswith((".pyc", ".pyo")) or "__pycache__/" in p)
        if _generated:
            print("Regression source check FAILED: generated artifacts are tracked in git")
            for path in _generated:
                print(f"  {path}")
            sys.exit(1)
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
    require_text(cfg_glue_cpp, "ProcessDynamicCodePolicy", "process startup prohibits JIT-style writable executable code")
    require_text(cfg_glue_cpp, "ProcessExtensionPointDisablePolicy", "process startup rejects extension-point DLL injection")
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
    state_sync_cpp = main_state_sync_cpp
    service_runtime_cpp = main_service_runtime_cpp
    require_text(main_data_paths_cpp, "resolve_service_machine_data_dir", "service machine data dir resolves under LocalAppData")
    require_text(main_data_paths_cpp, "service_cleanup_legacy_programdata", "legacy ProgramData directory cleanup exists")
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
    # Both containment roots named explicitly, so the machine scope added for the
    # update cache cannot later be "fixed" back into no scope at all.
    require_text(secure_write_cpp, "parent directory verified", "service file-write verifies the parent directory before writing")
    require_text(secure_write_cpp, "service_path_is_within_resolved_profile", "caller-scoped writes stay inside the authenticated caller's profile")
    require_text(secure_write_cpp, "service_path_is_within_machine_config", "machine-scoped writes stay inside the machine config directory")
    require_text(secure_write_cpp, "FILE_FLAG_OPEN_REPARSE_POINT", "service file-write opens parent without following reparse points")

    # F-01-002: Fan failure triggers rollback of earlier hardware writes
    require_text(gpu_backend_apply_cpp, "fan failure triggered rollback", "fan apply failure triggers rollback of earlier hardware writes")

    # F-01-003: Multi-GPU ordinal fallback blocked
    require_text(runtime_nvml_cpp, "refusing ordinal fallback", "multi-GPU ordinal fallback is blocked when PCI identity is available")

    # F-06-001: Lock capture always consumes the main-thread draft value, not a
    # stale lockedFreq fallback or HWND text from a different state generation.
    require_text(main_runtime_control_cpp,
        "g_app.guiDraft.curveValueValid[lockCi]",
        "lock capture validates the independent draft value")
    forbid_text(main_runtime_control_cpp, "get_window_text_safe(",
        "runtime apply capture never scrapes HWND text")

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

    require_text(service_runtime_cpp, "Service control is restricted to the active interactive session", "service rejects non-active-session callers (F-SEC-3 server-side)")
    require_text(service_runtime_cpp, "get_pipe_client_identity", "caller session is resolved from the pipe handle, not the payload")
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
    # Retired per-session ACL recycling (removed 2026-08-22); pin the broad ACL.
    require_text(service_server_cpp, "(A;;GRGW;;;AU)", "broad transition-safe pipe ACL is preserved")
    require_text_count(service_server_cpp, "FILE_FLAG_FIRST_PIPE_INSTANCE", 1,
                       "only one listener creation claims first-pipe-instance")
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
    require_text(os.path.join(SOURCE_DIR, "main_cli_help.cpp"), "--publish-slot-to-machine", "CLI supports publishing a slot to the shared bank")
    require_text(os.path.join(SOURCE_DIR, "main_cli_help.cpp"), "--clear-machine-slot", "CLI supports clearing a shared bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_PUBLISH_ID", "GUI advanced menu can publish a bank slot")
    require_text(ui_main_window_cpp, "MACHINE_LOGON_MENU_CLEAR_MACHINE_SLOT_ID", "GUI advanced menu can clear a bank slot")
    # F-15-008: One coherent "share with all users" action couples publishing the
    # slot data with setting it as the all-users default (the old footgun was
    # setting a default that resolved to an empty bank slot).
    require_text(config_profiles_machine_cpp, "share_profile_slot_for_all_users", "coherent share helper publishes data AND sets the default")
    require_text(config_profiles_machine_cpp, "unshare_profile_slot_for_all_users", "coherent unshare helper clears data AND the default")
    require_text(os.path.join(SOURCE_DIR, "main_cli_help.cpp"), "--share-slot", "CLI supports the coherent share action")
    require_text(os.path.join(SOURCE_DIR, "main_cli_help.cpp"), "--unshare-slot", "CLI supports the coherent unshare action")
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
    require_text(logon_startup_cpp,
                 "gui_service_model_ready(&g_app.guiServiceModel)",
                 "logon tray GUI observes only the accepted coherent service state")
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
                              "gui_service_retry_full_sync(\"reconnect timer\")",
                              "reconnect timer silently requests one coherent full snapshot")
    forbid_text_in_operation(ui_main_window_cpp,
                             "if (wParam == SERVICE_RECONNECT_TIMER_ID)",
                             "refresh_background_service_state()",
                             "reconnect timer never blocks the window thread on IPC")
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
    require_text(lifecycle_events_cpp, "gc_FindFirstChangeNotificationUtf8(",
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
                 "gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard",
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
    require_text(main_data_paths_cpp, "TokenStatistics",
                 "session identity includes the authentication LUID")
    require_text(main_data_paths_cpp, "AuthenticationId",
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
    require_text(os.path.join(SOURCE_DIR, "main_cli_help.cpp"),
                 "--set-restrict-shared", "CLI toggles the shared-only policy")
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
    require_text(ui_main_window_cpp, "LOGON_COMBO_SHARED_FLAG", "Logon dropdown offers admin shared profiles via item-data tags")
    require_text(ui_main_window_cpp, "CB_GETITEMDATA", "Logon dropdown handler decodes the selected item's meaning from item data")
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
                              "gc_WritePrivateProfileStringUtf8(nullptr, nullptr, nullptr, path)",
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
    require_text(ui_main_window_cpp,
                 "completedGeneration != currentGeneration",
                 "stale async startup repair schedules current-state reconciliation")
    require_text(config_profiles_ui_cpp, "Always use: Shared profile %d", "Logon dropdown lists admin-published shared profiles")
    require_text(config_profiles_ui_cpp, "Use admin's default (Shared profile %d)", "Logon dropdown shows the effective all-users default when the account has no choice")
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
    ui_main_graph_cpp = os.path.join(SOURCE_DIR, "ui_main_graph.cpp")
    require_text(ui_main_graph_cpp, "displayed_curve_mhz_for_gui_point", "GUI graph renders curve points from live driver readback")
    require_text(ui_main_graph_cpp, "gui locked tail live readback drift:", "GUI logs live tail readback drift diagnostics (no longer hidden)")
    require_text(desired_settings_helpers_cpp, "desired_is_fan_only_apply_request", "fan-only apply requests are detected without curve/OC fields")
    require_text(desired_settings_helpers_cpp, "desired_updates_curve_or_gpu_offset_state", "memory/power-only applies do not replace sparse curve intent")
    require_text(main_service_runtime_cpp, "merged fan-only request into active desired", "service fan-only applies preserve active curve intent")
    require_text(gpu_backend_cpp, "skipped VF edit repaint for fan-only apply", "GUI client fan-only applies do not clear sparse curve masks")
    require_text(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
                 "gui_service_accept_response_on_main_thread",
                 "mutation completion rebases from the service's complete active intent, preserving VF state after fan-only apply")
    require_text(main_runtime_control_cpp, "curvePoints=%d (%s)", "GUI capture logs sparse curve point list")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"), "point74=%d/%u point75=%d/%u point76=%d/%u", "profile save logs edited pre-tail VF points")
    require_text(main_runtime_capture_cpp, "capture_gui_desired_settings(&resetFull, true, true, false", "apply capture keeps sparse VF curve intent")
    require_text(main_runtime_capture_cpp, "capture_gui_desired_settings(&guiDesired, true, true, false", "profile save capture keeps sparse VF curve intent")
    require_text(config_profile_repair_cpp, "profile repair: removed non-tail readback artifact", "profile load repairs logged non-tail readback artifacts")
    require_text(main_gpu_state_cpp, "skippedLockedTail ? 4 : 3", "selective GPU offset detection rejects two-point high-edit false positives")

    # Lock mode (flatten/pin) must round-trip through profile save. The pin (hard)
    # mode was previously dropped because merge_desired_settings ignored lock state
    # and capture_gui_config_settings forgot lockMode. These would fail before the fix.
    require_text(config_profiles_ui_cpp, "base->lockMode = override->lockMode", "Windows merge_desired_settings carries lock mode")
    require_text(os.path.join(SOURCE_DIR, "linux_port_profiles.cpp"), "base->lockTracksAnchor = incoming->lockTracksAnchor", "Linux merge_desired_settings carries lock anchor tracking")
    require_text(main_runtime_capture_cpp, "full.lockMode = guiDesired.lockMode", "profile-save capture preserves lock mode from GUI")
    require_text(main_runtime_capture_cpp, "full.lockMode = g_app.lockMode", "profile-save capture preserves live lock mode")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"), "lock writing ci=", "profile save logs the lock ci/mhz/mode being written")
    require_text(ui_main_window_cpp, "show_lock_context_menu", "lock checkbox right-click mode menu exists")
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
    require_text(os.path.join(SOURCE_DIR, "ui_main_control_lifecycle.cpp"),
                 "gui_top_level_redraw_begin",
                 "edit-control rebuild uses the visibility-safe redraw transaction")
    require_text(os.path.join(SOURCE_DIR, "gui_service_state.cpp"),
                 "rebuild_edit_controls",
                 "service-state control rebuild routes through the redraw-suppressed render transaction")
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
    # Was pinned to linux_port.cpp's private parse_int_strict; that duplicate is
    # gone and both platforms now link the config_text_utils.cpp one.
    require_text(os.path.join(SOURCE_DIR, "config_text_utils.cpp"), "errno == ERANGE",
                 "the shared config integer parser rejects C library overflow")

    # F-LNX: native Linux GPU backend + daemon invariants
    linux_gpu_cpp = os.path.join(SOURCE_DIR, "linux_gpu.cpp")
    _linux_backend_surface = os.path.join(BUILD_WORK_DIR, "_linux_backend_surface.cpp")
    with open(_linux_backend_surface, "w", encoding="utf-8", errors="ignore") as _lf:
        for _cpp in (os.path.join(SOURCE_DIR, "linux_backend.cpp"),
                     os.path.join(SOURCE_DIR, "linux_backend_nvml_write.cpp"),
                     os.path.join(SOURCE_DIR, "linux_backend_discovery.cpp"),
                     os.path.join(SOURCE_DIR, "linux_backend_mutation.cpp")):
            with open(_cpp, "r", encoding="utf-8", errors="ignore") as _source:
                _lf.write(_source.read())
                _lf.write("\n")
    linux_backend_cpp = _linux_backend_surface
    linux_daemon_cpp = os.path.join(SOURCE_DIR, "linux_daemon.cpp")
    linux_daemon_lifecycle_h = os.path.join(SOURCE_DIR, "linux_daemon_lifecycle.h")
    linux_daemon_serve_h = os.path.join(SOURCE_DIR, "linux_daemon_serve.h")
    linux_fan_runtime_h = os.path.join(SOURCE_DIR, "linux_fan_runtime.h")
    # Logical daemon surface: the aggregator plus the shards it #includes.
    linux_daemon_surface = [linux_daemon_cpp, linux_daemon_lifecycle_h,
                            linux_daemon_serve_h, linux_fan_runtime_h]
    linux_daemon_transport_cpp = os.path.join(
        SOURCE_DIR, "linux_daemon_transport.cpp")
    linux_daemon_transport_policy_h = os.path.join(
        SOURCE_DIR, "linux_daemon_transport_policy.h")
    linux_systemd_notify_cpp = os.path.join(
        SOURCE_DIR, "linux_systemd_notify.cpp")
    linux_systemd_notify_policy_h = os.path.join(
        SOURCE_DIR, "linux_systemd_notify_policy.h")
    linux_daemon_state_cpp = os.path.join(SOURCE_DIR, "linux_daemon_state.cpp")
    linux_daemon_snapshot_cpp = os.path.join(SOURCE_DIR, "linux_daemon_snapshot_runtime.cpp")
    linux_service_install_cpp = os.path.join(SOURCE_DIR, "linux_service_install.cpp")
    linux_gpu_selection_h = os.path.join(SOURCE_DIR, "linux_gpu_selection.h")
    linux_gpu_binding_policy_h = os.path.join(
        SOURCE_DIR, "linux_gpu_binding_policy.h")
    linux_architecture_policy_h = os.path.join(
        SOURCE_DIR, "linux_architecture_policy.h")
    linux_vf_validation_h = os.path.join(
        SOURCE_DIR, "linux_vf_validation.h")
    linux_mutation_authority_h = os.path.join(
        SOURCE_DIR, "linux_mutation_authority.h")
    linux_port_profiles_cpp = os.path.join(SOURCE_DIR, "linux_port_profiles.cpp")
    linux_tui_actions_cpp = os.path.join(SOURCE_DIR, "linux_tui_actions.cpp")
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
    require_text(linux_gpu_selection_h, "low == nvidiaVendorId",
                 "Linux PCI matching recognizes NVIDIA vendor/device low-word encoding")
    require_text(linux_gpu_selection_h, "high == nvidiaVendorId",
                 "Linux PCI matching recognizes NVIDIA vendor/device high-word encoding")
    require_text(linux_daemon_snapshot_cpp,
                 "linux_gpu_switch_preserves_active_intent",
                 "Linux daemon cannot move one active intent onto another selected GPU")
    require_text(os.path.join(SOURCE_DIR, "linux_daemon_identity.cpp"),
                 "g_gpu.writeIdentityResolved",
                 "Linux daemon cannot publish a telemetry fallback as a selected multi-GPU write target")
    require_text(linux_tui_actions_cpp,
                 "linux_next_gpu_selection_index",
                 "Linux TUI requires an explicit first multi-GPU selection")
    require_text(linux_fan_runtime_h,
                 "&g_activeTarget, &g_gpu.selectedGpu",
                 "Linux fan worker verifies active intent ownership before hardware writes")
    require_text(linux_backend_cpp, "nvapiAssigned",
                 "Linux NvAPI handles map at most once to an NVML adapter")
    require_text(linux_backend_cpp, "g->writeIdentityResolved = g->adapterCount == 1 ||",
                 "Linux multi-GPU NVML writes require an exact selected identity")
    require_text(linux_backend_cpp, "g->gpuHandle = handles[g->nvapiIndex]",
                 "Linux VF capability requires a matched NvAPI handle")
    require_text(linux_main_cpp, "Cannot validate GPU selection: exact BDF/PCI identity",
                 "Linux CLI persists only a daemon-enriched stable GPU identity")
    require_text(linux_daemon_cpp, "startup reapply", "Linux daemon reapplies settings on (re)start")
    require_text(linux_daemon_transport_cpp, "GC_DAEMON_IO_TIMEOUT_MS", "Linux daemon socket I/O has bounded deadlines")
    require_text(linux_daemon_transport_cpp, "wait_fd_ready", "Linux daemon socket reads/writes poll with a deadline")
    require_text_in_surface(linux_daemon_surface, "set_nonblocking(conn)",
                            "Linux daemon accepted clients are nonblocking")

    # Fan failsafe/lifecycle, manual-write verification and the power-limit
    # range all live in tools/fan_gates.py.
    fan_gates.check_all(_gate_ctx(), require_text, forbid_text, require_order,
                        linux_backend_cpp)

    # Setup program: shared palette, upgrade ordering, and the settings transfer.
    installer_build.check_all(_gate_ctx(), require_text, forbid_text)

    # accept() failures must be classified; a dead listener must exit non-zero
    # or Restart=on-failure never restarts the daemon.
    require_text(linux_daemon_serve_h, "daemon_accept_disposition",
                 "Linux daemon classifies accept() failures")
    forbid_text_in_surface(linux_daemon_surface,
                           "if (errno == EINTR) continue; break;",
                           "Linux daemon no longer treats every accept() errno as fatal")
    require_text(linux_daemon_serve_h, "exitStatus = 1",
                 "a fatal listener failure exits non-zero for Restart=on-failure")
    require_text(linux_daemon_serve_h, "g_shutdownPipe[0]",
                 "the accept wait is broken by the shutdown self-pipe, not a timeout")
    require_text(linux_daemon_transport_policy_h, "DAEMON_ACCEPT_RECLAIM_FD",
                 "descriptor exhaustion has a dedicated non-spinning recovery")

    # --- Linux crash diagnostics parity (audit F-LNX-DIAG)
    # Linux builds with -fexceptions and uses std::string in the INI parser, so
    # bad_alloc/length_error could abort the root daemon with no journal entry.
    linux_crash_breadcrumb_h = os.path.join(SOURCE_DIR, "linux_crash_breadcrumb.h")
    linux_main_cpp_path = os.path.join(SOURCE_DIR, "linux_main.cpp")
    require_text(linux_crash_breadcrumb_h, "std::set_terminate",
                 "Linux installs a terminate handler for uncaught exceptions/bad_alloc")
    require_text(linux_crash_breadcrumb_h, "SA_RESETHAND",
                 "fatal-signal breadcrumbs restore default disposition before re-raising")
    require_text(linux_crash_breadcrumb_h, "raise(signalNumber)",
                 "the breadcrumb re-raises instead of suppressing the crash")
    # Only write(2) is async-signal-safe here; formatting helpers must not creep in.
    forbid_text(linux_crash_breadcrumb_h, "snprintf",
                "crash breadcrumb formatting stays async-signal-safe")
    forbid_text(linux_crash_breadcrumb_h, "fprintf",
                "crash breadcrumb output stays async-signal-safe")
    forbid_text(linux_crash_breadcrumb_h, "strsignal",
                "crash breadcrumb avoids non-async-signal-safe strsignal")
    require_text(linux_main_cpp_path, "linux_install_crash_breadcrumbs(\"cli\")",
                 "Linux CLI installs crash breadcrumbs before any work")
    require_text(linux_daemon_cpp, "linux_install_crash_breadcrumbs(\"daemon\")",
                 "Linux daemon relabels crash breadcrumbs for the root role")
    require_text(linux_daemon_cpp, "linux_set_crash_phase(\"daemon-serving\")",
                 "daemon crash breadcrumbs carry the current lifecycle phase")

    # systemd sandboxing: the daemon has no network path and no business in
    # /home, but it does need the NVIDIA devices and its own state directories.
    for directive in ("ProtectSystem=full", "ProtectHome=yes", "PrivateTmp=yes",
                      "RestrictAddressFamilies=AF_UNIX", "RestrictNamespaces=yes",
                      "RestrictSUIDSGID=yes", "SystemCallArchitectures=native",
                      "LockPersonality=yes"):
        require_text(linux_service_install_cpp, directive,
                     f"systemd unit applies {directive}")
    # These would break the NVIDIA user-mode stack; keep them out deliberately.
    forbid_text(linux_service_install_cpp, "PrivateDevices=yes",
                "unit must not hide /dev/nvidia* from the daemon")
    forbid_text(linux_service_install_cpp, "MemoryDenyWriteExecute=yes",
                "unit must not deny W+X to the NVIDIA user-mode stack")
    forbid_text(linux_service_install_cpp, "ProtectSystem=strict",
                "unit must not use strict ProtectSystem; the driver reads /usr and /sys")
    require_text(linux_daemon_cpp, "linux_daemon_state_remove", "Linux reset durably removes committed state")
    require_text(linux_service_install_cpp, "GC_INSTALL_BIN", "Linux systemd unit uses a protected staged daemon binary")
    require_text(linux_service_install_cpp, "root_owned_nonwritable_path", "Linux service install validates root-owned non-writable parents")
    require_text(linux_service_install_cpp, "stage_service_binary", "Linux service install stages the daemon binary before writing the unit")
    require_text(linux_service_install_cpp, "ExecStart=%s --daemon", "Linux systemd unit launches the staged daemon path")
    require_text(linux_service_install_cpp, "Type=notify", "Linux systemd unit waits for explicit daemon readiness")
    require_text(linux_service_install_cpp, "linux_service_run_activation", "Linux service activation uses the injected deterministic sequence")
    require_text(linux_service_install_cpp, "serviceBuildNumber", "Linux install verifies the restarted daemon build")
    forbid_text(linux_service_install_cpp, "enable\", (char*)\"--now", "Linux service upgrade never leaves an already-running old process resident")
    require_text(linux_service_install_cpp, "run_root_command", "Linux service management avoids shell command execution")
    forbid_text(linux_service_install_cpp, "system(", "Linux service management never invokes a shell")
    require_text(linux_daemon_transport_cpp, "int connectErrno = 0", "Linux daemon client preserves socket connection errno")
    require_text(linux_daemon_transport_cpp, "connectErrno == EACCES || connectErrno == EPERM", "Linux daemon client identifies socket permission denial")
    require_text(linux_daemon_transport_cpp, "char connectErrorText[128]",
                 "Linux permission diagnostics copy strerror text into owned storage")
    require_text(linux_daemon_transport_cpp, "facts.connectError = connectErrorText",
                 "Linux permission facts never retain strerror static storage")
    require_text(linux_daemon_transport_policy_h, "sudo usermod -aG greencurve", "Linux socket permission denial explains group enrollment")
    require_text(linux_daemon_transport_policy_h, "supplementary greencurve", "Linux permission diagnostics report supplementary group membership")
    require_text(linux_daemon_transport_cpp, "response header read", "Linux transport diagnoses header-first response failures")
    require_text(linux_daemon_transport_cpp, "protocol mismatch", "Linux transport diagnoses old daemon protocols before reading their body")
    require_text(linux_systemd_notify_policy_h, "READY=1", "Linux daemon sends systemd readiness only after startup")
    require_order(linux_daemon_cpp, "startup reapply", "daemon_open_listener(&listener)",
                  "Linux socket listening follows startup replay")
    require_text(linux_daemon_serve_h, "listen(srv, GC_DAEMON_LISTEN_BACKLOG)",
                 "Linux listener uses the named backlog constant")
    # The native fixture overrides GC_DAEMON_SOCKET_PATH, so it cannot include
    # linux_daemon.h and mirrors the backlog instead.  Keep the two in sync.
    linux_daemon_h = os.path.join(SOURCE_DIR, "linux_daemon.h")
    transport_fixture_cpp = os.path.join(SCRIPT_DIR, "tests",
                                         "linux_transport_regression.cpp")
    require_text(linux_daemon_h, "#define GC_DAEMON_LISTEN_BACKLOG 8",
                 "daemon backlog constant has its pinned value")
    require_text(transport_fixture_cpp, "#define GC_DAEMON_LISTEN_BACKLOG 8",
                 "native transport fixture mirrors the daemon backlog constant")
    require_text(transport_fixture_cpp, "daemon_accept_error_is_fatal",
                 "native fixture covers accept() failure classification")
    require_order(linux_daemon_cpp, "daemon_open_listener(&listener)",
                  "linux_systemd_notify_ready",
                  "Linux READY notification follows socket listening")
    require_text(linux_main_cpp, "installed, restarted, and verified", "Linux install success message reflects the deterministic restart and ping")
    require_text(linux_gpu_binding_policy_h, "LINUX_GPU_MATCH_SOLE_NONCONFLICTING", "Linux sole-GPU NvAPI fallback is explicit and testable")
    require_text(linux_gpu_binding_policy_h, "observation->extDeviceId",
                 "Linux binding can corroborate NVML with NvAPI's external PCI device ID")
    require_text(linux_backend_cpp, "nvapiExtDevice=%08x",
                 "Linux binding failure logs both internal and external NvAPI device IDs")
    require_text(linux_architecture_policy_h, "NVML_DEVICE_ARCH_BLACKWELL", "Linux architecture fallback maps documented NVML Blackwell")
    require_text(linux_vf_validation_h, "Live frequency can legitimately fall", "Linux VF ABI validation does not reject valid non-monotonic live clocks")
    require_text(linux_mutation_authority_h, "service_mutation_domains_require_vf", "Linux topology attachment is scoped to VF-dependent mutations")
    require_text(linux_main_cpp,
                 "The daemon socket pathname was verified root:greencurve mode=0660",
                 "Linux service install reports verified socket pathname authorization")
    require_text(linux_main_cpp, "sudo usermod -aG greencurve", "Linux help and generated assets document group enrollment")
    require_text(linux_port_profiles_cpp, 'addControl("lock_mode", value);',
                 "Linux profiles persist flatten versus hard-pin lock mode")
    require_text(linux_port_profiles_cpp, '"lock_tracks_anchor"',
                 "Linux profiles persist lock anchor tracking")
    require_text(linux_port_profiles_cpp,
                 "profile_slot_reference_after_clear(appLaunchSlot, slot, 0)",
                 "Linux profile clear removes stale app-launch references")
    require_text(linux_port_profiles_cpp,
                 "profile_slot_reference_after_clear(logonSlot, slot, 0)",
                 "Linux profile clear removes stale logon references")

    # Protocol-v12 retry safety, coherent state, and GUI-thread ownership.
    operation_tracker_h = os.path.join(SOURCE_DIR, "service_operation_tracker.h")
    gui_mutation_worker_cpp = os.path.join(SOURCE_DIR, "gui_mutation_worker.cpp")
    gui_mutation_policy_h = os.path.join(SOURCE_DIR, "gui_mutation_queue_policy.h")
    gui_service_io_policy_h = os.path.join(SOURCE_DIR, "gui_service_io_queue_policy.h")
    gui_service_model_h = os.path.join(SOURCE_DIR, "gui_service_model.h")
    gui_draft_policy_h = os.path.join(SOURCE_DIR, "gui_draft_policy.h")
    gui_tray_policy_h = os.path.join(SOURCE_DIR, "gui_tray_callback_policy.h")
    gui_tray_visibility_cpp = os.path.join(SOURCE_DIR, "gui_tray_visibility.cpp")
    gui_service_state_cpp = os.path.join(SOURCE_DIR, "gui_service_state.cpp")
    gui_selected_gpu_pnp_cpp = os.path.join(SOURCE_DIR, "gui_selected_gpu_pnp.cpp")
    ui_mutation_completion_cpp = os.path.join(
        SOURCE_DIR, "ui_mutation_completion.cpp")
    service_state_envelope_cpp = os.path.join(SOURCE_DIR, "main_service_state_envelope.cpp")
    require_text(service_protocol_h, "SERVICE_CMD_GET_OPERATION_RESULT",
                 "protocol exposes a read-only operation-result query")
    require_text(service_protocol_h, "gc_u64 operationId",
                 "mutating requests carry a 64-bit operation identity")
    require_text(operation_tracker_h, "SERVICE_OPERATION_HISTORY_CAPACITY 16",
                 "services retain the bounded latest-16 mutation cache")
    require_text(operation_tracker_h, "SERVICE_OPERATION_BEGIN_DUPLICATE",
                 "operation tracker deduplicates repeated mutation IDs")
    require_text(os.path.join(SOURCE_DIR, "main_service_operation_persist.cpp"),
                 "operation outcome became uncertain across service restart",
                 "Windows in-progress operations restore as outcome-unknown")
    require_text(main_service_persist_cpp, "SERVICE_ACTIVE_DESIRED_VERSION 5u",
                 "Windows protected active-state schema is version 5")
    require_text(main_service_persist_cpp, "SERVICE_ACTIVE_DESIRED_LEGACY_VERSION 4u",
                 "Windows active-state reader remains compatible with version 4")
    require_text(os.path.join(SOURCE_DIR, "linux_operation_runtime.h"),
                 "persist_daemon_operation",
                 "Linux persists both in-progress and completed correlation")
    require_text(gui_mutation_worker_cpp, "service_client_execute_mutation_request",
                 "GUI worker performs transport-only mutation execution")
    require_text(gui_mutation_worker_cpp, "APP_WM_MUTATION_COMPLETE",
                 "GUI worker posts completion to the main window")
    require_text(gui_mutation_worker_cpp, "gui_mutation_work_context_is_current",
                 "pending mutations revalidate session and GPU epoch")
    require_text(gui_mutation_policy_h, "GUI_MUTATION_QUEUE_KEEP_PENDING_RESET",
                 "a pending Reset cannot be overtaken by Apply")
    require_text(service_protocol_h, "struct ServiceStateEnvelope",
                 "every service response carries a generation-stamped envelope")
    require_text(service_protocol_h, "gc_u64 expectedServiceInstanceId",
                 "GUI mutations bind to the accepted service instance")
    require_text(service_protocol_h, "gc_u64 expectedGpuGeneration",
                 "GUI mutations bind to the accepted GPU generation")
    require_text(service_protocol_h, "gc_u64 expectedTopologySignature",
                 "GUI mutations bind to the accepted full topology")
    require_text(service_protocol_h, "service_snapshot_topology_signature",
                 "protocol owns one complete topology signature")
    require_text(service_protocol_h, "for (int i = 0; i < VF_NUM_POINTS; ++i)",
                 "topology hashing inspects every VF point")
    require_text(service_state_envelope_cpp, "BCryptGenRandom",
                 "Windows service instance IDs use the system CSPRNG")
    require_text(service_state_envelope_cpp, "populate_service_snapshot_locked",
                 "one lock capture produces the complete service envelope")
    require_text(service_state_envelope_cpp, "ImmutablePublishedServiceState",
                 "service publications replace one immutable complete state")
    require_order_in_operation(service_state_envelope_cpp,
                 "static void service_publish_gpu_phase(",
                 "AcquireSRWLockExclusive(&g_servicePublishedStateLock);",
                 "gc_u64 revision =",
                 "phase revision allocation is serialized in publication order")
    require_text(service_state_envelope_cpp,
                 "READY authority lost while publishing response",
                 "response-time authority loss advances the GPU generation")
    require_text(service_state_envelope_cpp,
                 "state.gpuPhase == SERVICE_GPU_PHASE_READY ? 1 : 0",
                 "cached adapter identity cannot resurrect live authority during recovery")
    require_text(os.path.join(SOURCE_DIR, "main_service_snapshot_request.cpp"),
                 "lostReadyAuthority",
                 "failed full refreshes retire the previous READY generation")
    require_text(main_service_selected_gpu_pnp_cpp,
                 "service_mark_selected_gpu_recovering",
                 "an arrival cue advances generation when removal was missed")
    require_text(service_state_envelope_cpp, "selectedIdentityMatches",
                 "mutation preconditions include the exact selected GPU")
    require_text(service_pipe_switch_cpp, "SERVICE_STATUS_STALE_STATE",
                 "stale mutations are rejected rather than crossing reconnect")
    require_text(service_pipe_cpp,
                 "if (stateEnvelopeAuthorized) populate_service_state_response(response);",
                 "only fully authorized Windows service callers receive the final state envelope")
    security_gates.check_ipc_transport_and_probe_gates(_gate_ctx(), require_text, forbid_text, require_text_count)
    require_text(gui_service_model_h, "minimumGpuGeneration",
                  "GUI PnP invalidation fences same-generation late responses")
    require_text(gui_service_model_h, "model->phase != GUI_SERVICE_READY",
                 "late PnP-start cues cannot fence an already-restarted recovering service")
    require_text(gui_service_model_h, "GUI_SERVICE_ENVELOPE_REJECTED_REVISION",
                 "GUI reducer rejects out-of-order completions")
    require_text(gui_service_model_h, "gui_service_failure_requires_render(",
                 "stable disconnected failures are presentation-idempotent")
    require_text(gui_draft_policy_h, "GUI_DRAFT_DETACH_DIRTY",
                 "dirty drafts detach on GPU/topology mismatch")
    require_text(gui_mutation_worker_cpp, "LONG presentationEpoch;",
                 "mutation completions are stamped with the presentation epoch")
    require_text(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
                 "gui_service_completion_context_current(",
                 "mutation completions revalidate their GPU epoch on the GUI thread")
    require_order(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
                  "GuiServiceModel previewModel",
                  "set_gui_state_dirty(false);",
                  "a pure reducer preview proves a mutation result current before the draft is cleared")
    require_text(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
                 "Unsaved draft preserved; synchronizing current state.",
                 "stale successful mutation completions preserve the independent draft")
    require_text(gui_service_io_policy_h,
                 "GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK",
                 "telemetry drops behind mutations, admin work, and full sync")
    require_text(gui_service_state_cpp, "gui_top_level_redraw_begin",
                 "accepted READY state renders in one visibility-safe redraw transaction")
    require_text(gui_service_state_cpp,
                 "gui_state_adoption_requires_redraw_suppression(",
                 "ordinary telemetry never suspends or repaints the whole window")
    require_text(gui_service_state_cpp, "if (renderChanged) gui_render_service_phase_only();",
                 "repeated transport failures repaint only when visible state changes")
    require_text(ui_main_window_cpp, 'gui_service_retry_full_sync("reconnect timer")',
                 "background reconnect probes preserve the current presentation")
    forbid_text(ui_main_window_cpp, 'gui_service_begin_full_sync("reconnect timer")',
                "background reconnect probes cannot flash the syncing presentation")
    require_text(os.path.join(SOURCE_DIR, "ui_control_projection.h"),
                 "gui_set_window_text_if_changed",
                 "service controls update text only when its projection changes")
    require_text(os.path.join(SOURCE_DIR, "ui_control_projection.h"),
                 "gui_set_window_enabled_if_changed",
                 "service controls update enablement only when it changes")
    forbid_text_in_operation(config_profiles_gui_state_cpp,
                "static void update_background_service_controls()", "UpdateWindow",
                "service status projection cannot force immediate child repaints")
    forbid_text_in_operation(config_profiles_gui_state_cpp,
                "static void update_background_service_controls()",
                "update_share_all_users_check_state();",
                "service status changes cannot redraw unrelated sharing controls")
    forbid_text_in_operation(config_profiles_ui_cpp,
                "static void update_share_all_users_check_state()", "UpdateWindow",
                "sharing control projection cannot force immediate child repaints")
    forbid_text(gui_service_state_cpp,
                "memcmp(&g_app.serviceActiveDesired",
                "telemetry projection changes use semantic intent equality rather than struct padding")
    forbid_text(gui_service_state_cpp, "RDW_ERASE",
                "coherent state adoption never erases the visible window before repaint")
    require_text(gui_tray_policy_h, "if (version4)",
                 "tray activation honors the negotiated shell callback version")
    require_text(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                 "duplicate open ignored while already visible",
                 "duplicate tray activation is idempotent")
    require_text(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                 "gui_set_window_text_if_changed(g_app.hFanEdit",
                 "stable fan telemetry does not repaint unchanged edit text")
    require_text(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                 "CB_GETCURSEL",
                 "stable fan telemetry does not reselect the current combo item")
    require_text(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                 "duplicate tray-autostart process exited without reopening",
                 "duplicate background tray startup cannot resurrect a hidden window")
    require_text(os.path.join(SOURCE_DIR, "single_instance_win32.cpp"),
                 "APP_WM_ACTIVATE_EXISTING_INSTANCE",
                 "explicit second-instance activation is routed through the resident GUI thread")
    forbid_text_in_operation(ui_main_window_cpp, "case WM_PAINT:",
                "fill_window_background(hwnd, hdc)",
                "backbuffer paint never clears the physical window before the final blit")
    require_text(gui_service_state_cpp, "g_app.visibleMap[vi]",
                 "GUI render topology includes every visible-map entry")
    require_text(gui_service_state_cpp, "bool canSelectGpu = ready",
                 "a detached draft can reselect its original GPU while writes stay blocked")
    require_text(gui_selected_gpu_pnp_cpp, "CM_Register_Notification",
                 "GUI subscribes to the exact selected-device PnP lifecycle")
    require_text(gui_selected_gpu_pnp_cpp,
                 "gui_service_model_require_new_gpu_generation",
                 "GUI PnP transitions require a newer service GPU generation")
    forbid_text(gui_mutation_worker_cpp, "apply_service_snapshot_to_app",
                "mutation worker cannot update application state off the GUI thread")
    forbid_text_in_operation(gui_mutation_worker_cpp,
                "static DWORD WINAPI gui_mutation_worker_proc(", "g_app.",
                "service I/O worker never mutates AppData")
    forbid_text_in_operation(service_connection_cpp,
                "static bool service_send_request(", "g_app.",
                "pipe transport never mutates GUI/application state")
    forbid_text_in_operation(
                os.path.join(SOURCE_DIR, "main_service_client_commands.cpp"),
                "static bool service_client_get_state_envelope(", "g_app.",
                "typed state reads return immutable results without GUI mutation")
    forbid_text(os.path.join(SOURCE_DIR, "main_service_client_commands.cpp"),
                "service_client_get_active_desired(",
                "clients consume active intent from the same full state envelope")
    require_text(os.path.join(SOURCE_DIR, "main_service_client_commands.cpp"),
                "g_app.serviceActiveDesiredValid = response.state.activeDesiredValid;",
                "compatibility mutation clients honor explicit envelope active-intent presence")
    forbid_text(os.path.join(SOURCE_DIR, "main_service_client_commands.cpp"),
                "response.snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE",
                "clients never infer active-intent validity from profile metadata")
    # F-SYNC-STAMP: the window path stamps mutations from GuiServiceModel; the
    # synchronous path (CLI, installer restore, --service-remove) carried
    # nothing and had every APPLY/RESET refused as malformed.
    client_commands_cpp = os.path.join(SOURCE_DIR, "main_service_client_commands.cpp")
    require_text(client_commands_cpp, "service_client_stamp_mutation_preconditions(&request,",
                 "synchronous mutations stamp the service state they name")
    require_order_in_operation(client_commands_cpp,
                 "static bool service_client_execute_mutation_request(",
                 "service_client_mutation_is_stamped(request)", "service_send_request(",
                 "an unstamped mutation is refused before it reaches the wire")
    require_text(main_state_sync_cpp, "service_client_identity_adopt(&g_syncClientStateIdentity",
                 "the synchronous path adopts the READY envelope it projects")
    require_text(main_state_sync_cpp, "service_client_identity_clear(&g_syncClientStateIdentity);",
                 "an unusable envelope drops the identity instead of leaving a stale stamp")
    require_text(os.path.join(SOURCE_DIR, "service_protocol_validation.h"),
                 "if (r->status != SERVICE_STATUS_OK && service_response_payload_is_absent(r))",
                 "a refusal that publishes no state reaches the client that caused it")
    require_text(os.path.join(SOURCE_DIR, "main_service_pipe.cpp"),
                 "service_request_reject_reason(request)",
                 "a refused request names the rule it broke in the service log")
    for runtime_gui_path in (
            ui_main_window_cpp,
            os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
            os.path.join(SOURCE_DIR, "main_fan_telemetry.cpp"),
            config_profiles_ui_cpp,
            config_profiles_gui_state_cpp,
            os.path.join(SOURCE_DIR, "config_profiles.cpp"),
            os.path.join(SOURCE_DIR, "auto_profile_win32.cpp"),
            os.path.join(SOURCE_DIR, "main_startup_profiles.cpp")):
        forbid_text(runtime_gui_path, "refresh_background_service_state(",
                    "runtime GUI paths cannot synchronously probe the service")
        forbid_text(runtime_gui_path, "service_client_get_snapshot(",
                    "runtime GUI paths cannot synchronously fetch snapshots")
        forbid_text(runtime_gui_path,
                    "refresh_service_snapshot_and_active_desired(",
                    "runtime GUI paths consume only accepted coordinator state")
    forbid_text(ui_main_window_cpp, "service_install_or_remove(",
                "window procedures cannot block on SCM service changes")
    forbid_text(ui_main_window_cpp, "launch_service_admin_helper(",
                "window procedures cannot wait on elevation helpers")
    forbid_text(gui_mutation_worker_cpp, "SetWindowText",
                "worker transport code cannot mutate HWND text")
    forbid_text(gui_mutation_worker_cpp, "EnableWindow",
                "worker transport code cannot mutate HWND capability state")
    require_text(ui_main_cpp, "CreateDIBSection(nullptr",
                 "retained GUI backbuffer lives in process-owned DIB memory")
    forbid_text(ui_main_cpp, "CreateCompatibleBitmap",
                "GUI cannot retain a display-driver-compatible bitmap across reconnect")
    require_text(ui_main_cpp, "g_backbufferGeneration",
                 "GDI surfaces are tied to an explicit display generation")
    require_text(ui_main_window_cpp,
                 "failure cannot leave stale pixels or create a repaint storm",
                 "BitBlt failure paints a direct fallback without a repaint storm")
    require_text(ui_main_window_cpp, "WM_DISPLAYCHANGE",
                 "display changes retire retained GDI state")
    require_text(ui_main_window_cpp, "WM_DWMCOMPOSITIONCHANGED",
                 "DWM composition changes retire retained GDI state")
    require_text(gui_mutation_worker_cpp, "CancelSynchronousIo",
                 "terminal shutdown cancels coordinator transport waits")
    require_order(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                   "gui_service_io_queue_telemetry(false)",
                   "ShowWindow(g_app.hMainWnd, SW_RESTORE)",
                   "tray reopen preserves a coherent cached first frame while refreshing asynchronously")
    require_text(app_shared_h, "bool trayWindowHiddenIntent;",
                 "tray-hidden state survives display-driver window reconstruction")
    require_text(ui_main_window_cpp, "case WM_WINDOWPOSCHANGING:",
                 "top-level visibility requests are policy-gated before display")
    require_text(ui_main_window_cpp, "position->flags &= ~SWP_SHOWWINDOW;",
                 "driver-induced top-level show is suppressed while tray-hidden")
    require_text(gui_tray_policy_h,
                 "gui_tray_hidden_intent_requires_rehide",
                 "tray-hidden policy covers visibility paths without SWP_SHOWWINDOW")
    require_text(ui_main_window_cpp,
                 "enforce_main_window_tray_state_from_message(hwnd, msg)",
                 "every main-window message enforces the durable tray-hidden postcondition")
    require_text(gui_tray_visibility_cpp,
                 "g_mainWindowTrayHideEnforcementActive",
                 "corrective tray hiding is guarded against nested ShowWindow messages")
    # The visibility-neutral projection transaction and the presentation-
    # preserving resync live in tools/ui_gates.py.
    require_text(gui_selected_gpu_pnp_cpp,
                 "enforce_main_window_tray_state",
                 "exact selected-GPU recovery reasserts tray residency")
    forbid_text_in_operation(os.path.join(SOURCE_DIR, "main_fan_runtime.cpp"),
                 "static void show_main_window_from_tray()",
                 'gui_service_begin_full_sync("tray window reopened")',
                 "tray reopen cannot flash a synchronizing overlay over coherent cached state")
    require_text(app_shared_h, "struct GuiDraft",
                 "GUI edits are stored independently from HWND text")
    require_text(app_shared_h, "bool pendingDesiredValid;",
                 "pre-READY sparse profile overlays remain explicit draft state")
    require_order_in_operation(gui_service_state_cpp,
                 "static void gui_apply_ready_envelope(",
                 "set_gui_state_dirty(false);",
                 "populate_desired_into_gui(&pendingDesired);",
                 "an unkeyed sparse draft rebases over READY before its overlay is restored")
    require_text(gui_service_state_cpp,
                 "rebased pre-READY sparse desired overlay on coherent live baseline",
                 "offline profile rebasing is change-diagnosable")
    require_order_in_operation(gui_service_state_cpp,
                 "static void gui_apply_ready_envelope(",
                 "begin_programmatic_edit_update();",
                 "apply_service_snapshot_to_app(&response->snapshot);",
                 "authoritative ready-state adoption suppresses synthetic edit notifications")
    require_order_in_operation(gui_service_state_cpp,
                 "static void gui_apply_ready_envelope(",
                 "gui_selected_gpu_notification_refresh(&g_app.selectedGpu);",
                 "end_programmatic_edit_update();",
                 "authoritative ready-state projection closes its programmatic-edit transaction")
    require_text(gui_service_state_cpp,
                 "new authority has no active desired intent; clean editor rebased",
                 "post-recovery no-intent state is explicit and diagnosable")
    forbid_text(ui_main_cpp, "GetWindowTextA(hEdit",
                "graph/editor rendering cannot scrape edit-control text")
    require_text(service_connection_cpp, "service_health_probe_should_defer",
                 "GUI health checks preserve the last proven service state during an owned mutation")
    require_text(os.path.join(SOURCE_DIR, "main_fan_telemetry.cpp"), "fan telemetry: deferred while the GUI owns an active GPU mutation",
                 "fan telemetry cannot misclassify the serialized service pipe during Apply/Reset")
    require_text(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
                 "completion->response.magic == SERVICE_PROTOCOL_MAGIC",
                 "a validated mutation response re-proves service availability without a redundant ping")

    # Windows identity and privileged output boundary.
    require_text(main_service_runtime_identity_cpp, "ImpersonateNamedPipeClient(pipe)",
                 "named-pipe identity comes from the connected client token")
    require_text(main_service_runtime_identity_cpp, "OpenThreadToken(GetCurrentThread()",
                 "pipe authorization opens the impersonated thread token")
    require_text(main_service_runtime_identity_cpp, "DuplicateTokenEx(threadToken",
                 "pipe authorization retains a stable duplicated token")
    require_order_in_operation(main_service_runtime_identity_cpp,
                  "static bool get_pipe_client_identity(",
                  "OpenThreadToken(GetCurrentThread()",
                  "impersonation.revert()",
                  "pipe identity always reverts before token metadata work")
    require_text(main_service_runtime_identity_cpp,
                 "class ScopedPipeClientImpersonation",
                 "pipe-client impersonation is reverted through RAII")
    require_text(service_server_cpp, "request->callerPid != caller->pid",
                 "payload caller PID must match the pipe-reported PID")
    require_text(service_server_cpp, "caller->integrityRid < SECURITY_MANDATORY_MEDIUM_RID",
                 "control and file-output requests reject low-integrity clients")
    require_text(service_server_cpp, "ScopedServiceClientImpersonation impersonation(callerToken)",
                 "privileged file output is written under caller impersonation")
    require_order(service_server_cpp,
                  "hardware_initialize(detail, sizeof(detail))",
                  "ScopedServiceClientImpersonation impersonation(callerToken)",
                  "hardware capture precedes caller-scoped destination writes")
    require_text(secure_write_cpp, "BCryptGenRandom",
                 "Windows secure-write temporary names use the OS CSPRNG")
    require_text(service_request_policy_cpp, "CompareStringOrdinal",
                 "Windows canonical containment uses ordinal UTF-16 comparison")

    # UTF-8 is the internal path encoding; all path/profile boundaries use W APIs.
    win32_utf8_paths_h = os.path.join(SOURCE_DIR, "win32_utf8_paths.h")
    require_text(win32_utf8_paths_h, "using Win32Utf8Path = GcWideUtf8Arg",
                 "internal Win32Utf8Path conversion type exists")
    require_text(win32_utf8_paths_h, "CP_UTF8, MB_ERR_INVALID_CHARS",
                 "UTF-8 path conversion is strict")
    require_text(win32_utf8_paths_h, "WritePrivateProfileStringW",
                 "profile writes cross the UTF-16 Win32 boundary")
    ansi_path_calls = (
        "CreateFileA(", "GetFileAttributesA(", "GetFileAttributesExA(",
        "SetFileAttributesA(", "DeleteFileA(", "MoveFileExA(",
        "CopyFileA(", "CreateDirectoryA(", "RemoveDirectoryA(",
        "GetFullPathNameA(", "GetFinalPathNameByHandleA(",
        "GetModuleFileNameA(", "GetSystemDirectoryA(",
        "GetPrivateProfileStringA(", "WritePrivateProfileStringA(",
        "GetPrivateProfileSectionA(", "WritePrivateProfileSectionA(",
        "FindFirstFileA(", "FindNextFileA(",
        "FindFirstChangeNotificationA(",
    )
    for name in os.listdir(SOURCE_DIR):
        if not name.endswith((".cpp", ".h")):
            continue
        source_path = os.path.join(SOURCE_DIR, name)
        for call in ansi_path_calls:
            forbid_text(source_path, call,
                        f"production path API remains Unicode-safe: {name} has no {call}")

    # Linux runtime boundaries fail closed and fatal TUI exits restore termios.
    linux_socket_permissions_h = os.path.join(SOURCE_DIR, "linux_socket_permissions.h")
    linux_socket_path_permissions_h = os.path.join(
        SOURCE_DIR, "linux_socket_path_permissions.h")
    linux_tui_cpp = os.path.join(SOURCE_DIR, "linux_tui.cpp")
    require_text_in_surface(linux_daemon_surface, "umask(0077)",
                            "daemon creates its runtime socket under restrictive umask")
    require_text(linux_socket_path_permissions_h,
                 "fstatat(directoryFd, socketName, &status",
                 "socket authorization verifies the filesystem pathname")
    require_text(linux_socket_path_permissions_h,
                 "fchownat(directoryFd, socketName, expectedOwner, expectedGroup",
                 "socket pathname ownership is updated relative to the protected directory")
    require_text(linux_socket_path_permissions_h,
                 "fchmodat(directoryFd, socketName, expectedMode & 0777, 0)",
                 "socket pathname mode is updated relative to the protected directory")
    require_text(linux_socket_permissions_h, "group ? 0660 : 0600",
                 "missing greencurve group deliberately falls back to root-only")
    forbid_text(linux_socket_permissions_h, "fchown(socketFd",
                "socket descriptor ownership never substitutes for pathname ownership")
    forbid_text(linux_socket_permissions_h, "fchmod(socketFd",
                "socket descriptor mode never substitutes for pathname mode")
    require_text(linux_service_install_cpp,
                 "LINUX_SERVICE_STEP_VERIFY_SOCKET",
                 "service installation verifies pathname authorization before protocol ping")
    require_text(linux_service_install_cpp, "UMask=0077",
                 "systemd service repeats restrictive umask hardening")
    require_text(linux_service_install_cpp, "RuntimeDirectoryMode=0755",
                 "systemd creates the protected socket directory deterministically")
    require_text(linux_tui_cpp, "pid_t child = fork()",
                 "Linux TUI runs under a terminal-restoring supervisor")
    require_text(linux_tui_cpp, "waitpid(child, &childStatus, 0)",
                 "TUI supervisor observes normal and fatal child termination")
    require_order(linux_tui_cpp, "waitpid(child, &childStatus, 0)",
                  "tcsetattr(STDIN_FILENO, TCSAFLUSH, &original)",
                  "terminal restoration happens in normal parent control flow")
    forbid_text(linux_tui_cpp, "on_fatal_signal",
                "fatal signal handlers never call terminal or stdio APIs")
    # F-LNX-TUI/EXIT/TERM/DEBUGLOG/STARTUP/PKG all live in tools/linux_gates.py.
    linux_gates.check_all(_gate_ctx(), require_text, forbid_text, require_order)

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
    require_text(linux_backend_cpp, "bool hardwareSnapshotOk = linux_backend_capture_snapshot",
                 "Linux self-test populates its capability report from a read-only snapshot")
    require_text(linux_backend_cpp, "linux_backend_self_test", "read-only driver/arch self-test exists")

    # F-CAP invariants must not regress working x64/discrete setups.
    capability_h = os.path.join(SOURCE_DIR, "gpu_capability_policy.h")
    capability_probe_cpp = os.path.join(SOURCE_DIR, "gpu_capability_probe.cpp")
    vf_backends_cpp_path = os.path.join(SOURCE_DIR, "vf_backends.cpp")
    require_text(capability_h, "GPU_DOMAIN_CAP_UNPROBED = 0",
                 "an unprobed domain is the zero value, so a zeroed probe subtracts nothing")
    require_text(vf_backends_cpp_path,
                 "gpu_capability_available_domains(nullptr) == SERVICE_MUTATION_DOMAIN_ALL",
                 "F-CAP core invariant is asserted at compile time")
    require_text(vf_backends_cpp_path,
                 "gpu_capability_mask_for_index(0) == SERVICE_MUTATION_DOMAIN_RESET_BASELINE",
                 "capability bit indices are pinned against the protocol domain bits")
    # The conservatism that keeps older x64 drivers quiet.
    require_text(capability_h, "if (!obs->entryPointPresent) return GPU_DOMAIN_CAP_UNPROBED;",
                 "a missing entry point means an older driver, never absent hardware")
    # The probe must stay read-only.
    for _write_api in ("setClockOffsets", "setPowerLimit", "setFanSpeed"):
        forbid_text(capability_probe_cpp, _write_api,
                    f"the capability probe never calls {_write_api}")
    # The legacy getter falsely refused both offset domains on a working RTX 5070.
    require_text(capability_probe_cpp, "nvml_get_offset_range",
                 "clock-offset capability is probed via the apply path's resolver")
    forbid_text(capability_probe_cpp, "g_nvml_api.getGpcClkMinMaxVfOffset(",
                "the probe must not call the legacy offset getter directly")
    require_text(capability_probe_cpp, "gpu_memory_topology_from_sizes",
                 "memory topology is classified from the reported dedicated/shared split")
    require_text(capability_h, "if (sharedBytes >= GPU_DEDICATED_VRAM_FLOOR_BYTES)",
                 "an empty/failed memory query stays unknown rather than unified")
    # Windows read-only pre-flight, the counterpart of the Linux self-test.
    self_test_cpp = os.path.join(SOURCE_DIR, "main_self_test.cpp")
    require_text(self_test_cpp, "DriverSelfTestFacts facts",
                 "the Windows self-test derives its verdict from every required read")
    require_text(self_test_cpp, "nvapi_read_curve", "the Windows self-test reads VF directly")
    require_text(self_test_cpp, "is_elevated", "the self-test distinguishes missing privilege")
    require_text(entry_cpp, "self_test_cli_dispatch(opts,", "the Windows self-test publishes a scriptable process exit code")
    forbid_text(self_test_cpp, "hardware_initialize",
                "the self-test must not depend on the service-mediated init path")
    require_text(main_state_sync_cpp, "snapshot->health.capabilityDomainsPacked =",
                 "the service publishes its capability probe")
    require_text(main_state_sync_cpp, "gpu_capability_unpack_domains(&g_app.gpuCapability",
                 "the GUI adopts service capability state for warnings and confirmation")
    require_text(os.path.join(SOURCE_DIR, "ui_main_apply.cpp"), "gpu_capability_memory_write_is_risky", "unified-memory confirmation is topology-gated")
    # arm64 needs native nvapia64.dll, not x64 emulation (verified on driver 616.00).
    require_text(os.path.join(SOURCE_DIR, "nvapi_module_policy.h"), "nvapia64.dll",
                 "arm64 prefers the native NVAPI image")
    require_text(os.path.join(SOURCE_DIR, "linux_backend_mutation.cpp"),
                 "gpu_capability_set(&g->capability",
                 "the Linux daemon mirrors its domain mask into the capability probe")
    require_text(os.path.join(SOURCE_DIR, "linux_gpu.cpp"),
                 "bool linux_platform_is_integrated_soc()",
                 "integrated-SoC detection exists exactly once and is shared")
    # Both warning tiers must remain independently suppressible.
    # A recognized discrete family cannot have a unified pool, so that pairing
    # is positive evidence of an integrated part even when every domain answers
    # -- the one way untested silicon could otherwise reach a user in silence.
    require_text(vf_backends_cpp_path, "gpu_capability_topology_contradicts_discrete",
                 "an integrated part reporting a discrete family still warns")
    require_text(os.path.join(SOURCE_DIR, "main_capability_warning.cpp"), "integratedOnly",
                 "the warning text distinguishes integrated-only from missing domains")
    require_text(main_gpu_front_cpp, "hide_limited_control_warning",
                 "limited control-surface warning has its own persistent opt-out")
    require_text(main_gpu_front_cpp, "g_limitedControlWarningShownThisSession",
                 "limited control-surface warning re-shows once per GUI session")
    require_text(os.path.join(SOURCE_DIR, "main_capability_warning.cpp"),
                 "show_limited_control_surface_warning",
                 "the second warning tier exists")
    require_text(os.path.join(SOURCE_DIR, "main_capability_warning.cpp"),
                 "show_gpu_support_warnings",
                 "both warning tiers are reached through one GUI startup call site")
    # Driver-inspection gates (incl. the NvAPI entry-point id scan) live with
    # the module they check; see tools/driver_inspect.py.
    driver_inspect.check_source_gates(_gate_ctx(), require_text)
    require_text(linux_backend_cpp,
                 "info.pstate = nvml_configured_clock_offset_pstate()",
                 "Linux modern NVML offsets always target configured P0")
    require_text(linux_backend_cpp, "NvmlClockOffsetReadback gpuOffset =",
                 "Linux rollback capture uses modern clock-offset readback")
    require_text(linux_backend_cpp, "graphicsDomainBoundaryLogged",
                 "Linux VF domain-boundary logging is transition-deduplicated")
    require_text(runtime_nvml_cpp,
                 "info.pstate = nvml_configured_clock_offset_pstate()",
                 "Windows modern NVML offsets always target configured P0")
    forbid_text(runtime_nvml_cpp, "statesToTry",
                "Windows NVML offsets never target a transient current P-state")
    require_text(os.path.join(SOURCE_DIR, "linux_live_output.cpp"),
                 "compare_intent_to_readback",
                 "Linux live exports compare configured intent with hardware")
    require_text(os.path.join(SOURCE_DIR, "linux_tui_layout.cpp"),
                 "HARDWARE OVERRIDDEN",
                 "Linux TUI visibly discloses external hardware overrides")
    # The v14 readback-validity contract has a producer on BOTH platforms. The
    # Windows service publishing all-zero bits would silently report every
    # domain as unavailable while still substituting intent for failed reads.
    state_sync_cpp = os.path.join(SOURCE_DIR, "main_state_sync.cpp")
    gpu_state_cpp = os.path.join(SOURCE_DIR, "main_gpu_state.cpp")
    gpu_backend_cpp_path = os.path.join(SOURCE_DIR, "gpu_backend.cpp")
    readback_policy_h = os.path.join(SOURCE_DIR, "control_readback_policy.h")
    require_text(state_sync_cpp, "apply_control_readback_validity(state, &facts);",
                 "the Windows service publishes per-domain readback validity")
    require_text(state_sync_cpp, "facts.gpuOffsetFromHardware = gpuOffsetFromHardware;",
                 "the Windows service publishes GPU offset readback provenance")
    require_text(state_sync_cpp, "facts.fanPolicyKnown = g_app.readback.fan.policy;",
                 "the Windows service publishes per-fan readback provenance")
    require_text(state_sync_cpp,
                 "merged.gpuOffsetReadbackValid = state->gpuOffsetReadbackValid;",
                 "the Windows GUI merge carries readback validity with its value")
    require_text(gpu_state_cpp,
                 "static int current_applied_gpu_offset_mhz(bool* fromHardware)",
                 "the Windows applied GPU offset reports whether it is a reading")
    require_text(gpu_state_cpp, "*fromHardware = false;  // remembered request",
                 "the persisted selective request is never reported as readback")
    require_text(gpu_state_cpp, "*fromHardware = false;  // active desired intent",
                 "the active-desired fallback is never reported as readback")
    require_text(gpu_backend_cpp_path,
                 "g_app.readback.gpuOffset = gpu_offset_readback_after_detection(",
                 "clock-offset detection owns the GPU scalar it overwrites")
    require_text(gpu_backend_cpp_path, "g_app.readback.powerLimit = true;",
                 "a Windows power reading records its own provenance")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
                 "invalidate_scalar_readbacks(&g_app.readback);",
                 "a rollback drops readback validity with the scalars it zeroes")
    require_text(readback_policy_h, "all_fans_known",
                 "a partially answering fan set is not a readback")
    require_text(os.path.join(SOURCE_DIR, "intent_readback_status.h"),
                 "diverged = true;\n                    continue;",
                 "a fan policy takeover is disclosed even when the duty getter "
                 "goes quiet with it")
    require_text(linux_gpu_cpp, "nv_tegra_release",
                 "probe detects unsupported Tegra/Jetson platform")
    # (aarch64 driver-tree inspection is gated in driver_inspect.check_source_gates)
    # Shared VfBackendSpec tables compiled on both platforms (one copy).
    require_text(vf_backends_cpp, "g_vfBackendBlackwell", "shared VfBackendSpec tables define the Blackwell backend")
    # glibc-dynamic Linux target (static musl can't dlopen the glibc driver libs).
    require_text(build_script, '"x86_64-linux-gnu"', "Linux target is glibc-dynamic (x86_64-linux-gnu)")
    # Multi-arch build matrix + release archiving.
    require_text(build_script, 'default="all"', "default build covers all OSes (windows + linux)")
    require_text(build_script, '"-target", "aarch64-windows-gnu"', "windows arm64 Zig build target triple defined")
    require_text(build_script, 'LINUX_ARM64_TRIPLE = "aarch64-linux-gnu"', "linux arm64 target triple defined")
    require_text(build_script, "-mbranch-protection=standard", "windows arm64 uses BTI/PAC instead of x86 CET")
    # Archive/manifest/packaging guards live with the manifest they describe.
    release_manifest_check_all(_gate_ctx(), require_text)
    require_text(build_script, "ARM64 branch protection missing", "final ARM64 binaries must contain BTI and PAC/AUT")
    require_text(build_script, "-mbranch-protection=standard", "arm64 builds enable BTI/PAC branch protection")
    # ARM64 VEH thread-redirect uses the aarch64 register names.

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
    require_text(main_runtime_capture_cpp,
        "static void capture_applied_curve_baseline(const DesiredSettings* desired)",
        "F-DRIFT-1: baseline is captured from intent (DesiredSettings), not live readback")
    require_text(main_runtime_capture_cpp,
        "g_app.appliedCurveMHz[i] = desired->curvePointMHz[i];",
        "F-DRIFT-1: baseline values come from the applied desired curve, not g_app.curve")
    # Fan-only apply detection compares the editor against the drift-free baseline,
    # NOT live readback, so expected boost drift on a pre-tail point can no longer
    # reclassify a fan-only change as a curve edit (the reported bug).
    require_text(main_runtime_capture_cpp,
        "unsigned int baselineMHz = g_app.appliedCurveMHz[i];",
        "F-DRIFT-1: fan-only curve-change detection uses the drift-free baseline")
    require_text(main_runtime_capture_cpp,
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
    require_text(os.path.join(SOURCE_DIR, "ui_mutation_completion.cpp"),
        "memset(g_app.appliedCurveMHz, 0,",
        "F-DRIFT-1: reset clears the owned VF curve intent baseline")

    # F-APPLY-SPEED: profile-switch speed levers must be gated with defaults that
    # preserve the exact current (TDR-safe) behaviour — the fast paths are opt-in and
    # require GPU validation. reset_settle_ms defaults to the historical 1000ms;
    # skip_reset_curve_write defaults to 0 (the reset-to-zero write still runs).
    reset_baseline_cpp = os.path.join(SOURCE_DIR, "gpu_backend_reset_baseline.cpp")
    require_text(os.path.join(SOURCE_DIR, "gpu_backend_apply.cpp"),
        "get_config_int(g_app.configPath, \"apply\", \"reset_settle_ms\", 1000)",
        "F-APPLY-SPEED: TDR settle is tunable but defaults to the historical 1000ms")
    # The reset-curve-write is load-bearing for the delta-based selective/boost apply
    # (excluded points collapse to originalCurveOffsets[ci], so a skipped reset strands
    # the previous profile's offset on them) and must NOT be gated/skippable.
    forbid_text(reset_baseline_cpp, "\"apply\", \"skip_reset_curve_write\"",
        "F-APPLY-SPEED: the VF-curve reset-to-zero must not be skippable (delta-boost baseline)")
    require_text(reset_baseline_cpp,
        "if (hadCurveOffsets && !apply_curve_offsets_verified(resetOffsets, resetMask, 2))",
        "F-APPLY-SPEED: the VF-curve reset-to-zero always runs when the previous profile had curve offsets")
    require_text(reset_baseline_cpp,
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

    # F-PRESENTATION-SILENT: a tray or automatic profile switch is background
    # work. Its apply and completion paths may update the resident model and
    # deferred paint state, but must not create/show UI, activate/focus/flash a
    # window, allocate a console, or launch another process. This prevents both
    # visible ghost surfaces and focus theft from rule-driven auto-profiles.
    presentation_surface_tokens = (
        "MessageBox", "gc_message_box", "TaskDialog", "DialogBox", "CreateDialog",
        "CreateWindow", "ShowWindow", "AnimateWindow", "SetWindowPos",
        "SetForegroundWindow", "SetActiveWindow", "BringWindowToTop",
        "SetFocus", "AllowSetForegroundWindow", "AttachThreadInput",
        "SwitchToThisWindow", "FlashWindow", "SWP_SHOWWINDOW",
        "APP_WM_ACTIVATE_EXISTING_INSTANCE", "ShellExecute", "CreateProcess",
        "WinExec", "AllocConsole")
    presentation_silent_operations = (
        (auto_win32_cpp, "static bool ap_do_apply_slot("),
        (auto_win32_cpp, "static void auto_profile_on_mutation_completed("),
        (ui_mutation_completion_cpp,
         "static void handle_auto_profile_mutation_completion_presentation_silent("))
    for path, operation in presentation_silent_operations:
        for token in presentation_surface_tokens:
            forbid_text_in_operation(
                path, operation, token,
                "F-PRESENTATION-SILENT: background profile apply/completion "
                f"cannot use {token}")
    require_text_in_operation(
        ui_mutation_completion_cpp,
        "static void handle_auto_profile_mutation_completion_presentation_silent(",
        "auto_profile_on_mutation_completed(",
        "F-PRESENTATION-SILENT: auto-profile completion is isolated in its guarded handler")
    require_text_in_operation(
        ui_mutation_completion_cpp,
        "static void handle_gui_mutation_completion(",
        "handle_auto_profile_mutation_completion_presentation_silent(",
        "F-PRESENTATION-SILENT: background mutation completion uses the guarded handler")

    # F-AUTO-PROFILE: the driver is wired into the GUI lifecycle (init/shutdown),
    # the WM_HOTKEY path, and the unity build.
    auto_ui_cpp = ui_main_window_cpp
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
    require_text(main_service_controlled_restart_cpp, "wcsspn(textValue, L\"0123456789\")",
        "controlled helper accepts only unsigned decimal process and handle values")
    require_text(main_service_controlled_restart_cpp, "errno == ERANGE",
        "controlled helper rejects overflowing process and handle values")
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

    # Formatting and subprocess helpers must preserve their cursor/exit-status
    # contracts: both bugs silently discarded diagnostics in past releases.
    require_text(platform_h, "static inline size_t gc_appendf",
        "bounded formatted appends return a safe byte cursor")
    forbid_text(diagnostics_cpp, "+= StringCchPrintf",
        "crash breadcrumb assembly does not treat HRESULT as a character count")
    forbid_text(gpu_backend_apply_cpp, "+= StringCchPrintf",
        "apply summaries do not treat HRESULT as a character count")
    require_text(platform_win32_cpp, "GetExitCodeProcess",
        "Windows subprocess capture requires a zero child exit status")
    require_text(platform_posix_cpp, "WIFEXITED(status)",
        "POSIX subprocess capture requires a normal child exit")
    require_text(platform_posix_cpp, "WEXITSTATUS(status) == 0",
        "POSIX subprocess capture requires a zero child exit status")
    require_text(app_shared_cpp, "if (hdc)",
        "DPI fallback validates its screen device context before use")
    require_text(app_shared_cpp, "== 0 && dpiX > 0",
        "monitor DPI discovery rejects a zero scale result")
    forbid_text(main_service_recovery_cpp,
        "service_startup_coordinator_thread_proc",
        "retired per-start recovery thread stays removed")
    # F-REL-2e: after the service authoritatively resets to no-lock, the GUI clears
    # a stale ADOPTED lock (checkbox / point value / header) when the user is not
    # dirty-editing — otherwise the gate would pin it forever once lockedCi>=0.
    require_text(state_sync_cpp, "clearing stale adopted GUI lock",
        "GUI clears a stale adopted lock when the service reports authoritative no-lock and the user is not editing")
    require_text(os.path.join(SOURCE_DIR, "gui_service_state.cpp"),
        "g_guiForceFullRefresh",
        "accepted telemetry does a full coherent render transaction when reset clears a stale lock")
    # F-REL-2f: while minimized to the tray, keep a slow telemetry poll so the tray
    # icon/tooltip reflect service state changes (e.g. a reset) without opening the window.
    require_text(main_gpu_front_cpp, "TRAY_HIDDEN_POLL_INTERVAL_MS",
        "tray icon keeps a slow telemetry poll while the window is hidden")
    # F-REL-2g: the tray icon AND tooltip only report OC/fan/profile as active when
    # the GPU live state is actually available (tray_hardware_live()), so a disabled
    # driver / down service shows the default icon and a "GPU driver unavailable"
    # tooltip instead of a false active state for the merely-pending desired.
    # The tooltip/hardware-availability helpers live in tray_presentation.cpp,
    # which main_gpu_front.cpp includes; check the surface so the split holds.
    tray_surface = (main_gpu_front_cpp, os.path.join(SOURCE_DIR, "tray_presentation.cpp"))
    require_text_in_surface(tray_surface, "static bool tray_hardware_live()",
        "shared tray hardware-availability helper exists")
    require_text(main_fan_runtime_cpp, "tray_hardware_live()",
        "tray icon gates OC/fan-active on actual GPU availability (not a pending desired)")
    require_text_in_surface(tray_surface, "gui_service_phase_tray_text",
        "tray tooltip reports reducer phase instead of a false OC/fan/profile-active state")
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
    # Needs the generated service-server surface, which only exists here.
    require_text(service_server_cpp, "rotate_crash_artifacts_for_process();",
        "service startup rotates crash artifacts")

    # FP-06-006: the VEH is the crash DETECTOR only — it invalidates NVML without
    # nvmlShutdown and lets the main loop request the restart.
    require_text(main_service_runtime_cpp,
        "service_close_nvml_without_shutdown",
        "driver-crash recovery has a no-shutdown NVML invalidation helper")
    require_text(crash_artifacts_cpp,
        "service_close_nvml_without_shutdown();",
        "VEH crash path invalidates NVML without nvmlShutdown")
    forbid_text(crash_artifacts_cpp,
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
    # worker kill/recreate storm can't hard-crash the process via
    # STATUS_INVALID_HANDLE under Strict Handle Checks. The fixed per-worker
    # slot scheme publishes/unpublishes each instance handle atomically; only
    # the slot owner (worker or reaper) ever closes it.
    require_text(service_server_cpp,
        "void publish_worker_pipe(int index, HANDLE pipe)",
        "pipe workers atomically publish their instance handle into a fixed slot")
    require_text(service_server_cpp,
        "void retire_worker_pipe(int index)",
        "pipe handle retirement unpublishes the slot before closing")
    require_text(service_server_cpp,
        "InterlockedCompareExchange",
        "first-pipe-instance claim is atomic so a foreign listener fails startup")

    # F-03-005: service_lifecycle_identity_equal_session_and_user is used in the
    # sessions layer to handle auth LUID drift between identity sources.
    require_text(os.path.join(SOURCE_DIR, "main_service_sessions.cpp"),
        "service_lifecycle_identity_equal_session_and_user",
        "session code uses LUID-tolerant identity comparison for logon matching")

    # F-03-005: point visibility is preserved in profile save paths. Regression
    # guard for the "all points written as hidden" machine-config sharing bug.
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"),
        "point%d_visible=%d",
        "profile save emits per-point visibility flags")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"),
        "point%d_visible=%d\\r\\n",
        "profile save writes point visibility with explicit CRLF line endings")

    # F-03-005: no stale VF drift monitor — the continuous auto-reapply was
    # removed in 0.18 (was a TDR risk).
    forbid_text(runtime_nvml_cpp,
        "g_app.continuousVfDriftCheckRunning",
        "continuous VF drift monitor was removed (0.18)")
    forbid_text(runtime_nvml_cpp,
        "detect_and_correct_vf_drift",
        "VF drift auto-correction was removed (0.18)")

    # F-03-005: lockMode is propagated through all save paths (profile save
    # must include lock state to prevent dropped lock on save).
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"),
        "lockMode",
        "lock mode field is serialized in profile save/load")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"),
        "g_app.lockMode",
        "lock mode field from app state is serialized in profile save path")
    require_text(os.path.join(SOURCE_DIR, "config_profiles.cpp"),
        "desired->lockMode",
        "lock mode field from desired settings is serialized in profile save path")



def parse_args():
    parser = argparse.ArgumentParser(description="Build Green Curve targets with Zig")
    parser.add_argument(
        "--target",
        choices=("windows", "linux", "all"),
        default=None,
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
        "--fetch-toolchain",
        action="store_true",
        help="Download the pinned compiler/archiver archives into compilers/ and exit",
    )
    parser.add_argument(
        "--verify-toolchain",
        action="store_true",
        help="Verify the installed toolchain against compilers/*/manifest.json and exit",
    )
    parser.add_argument(
        "--toolchain-manifest",
        metavar="PATH",
        default=None,
        help="Write a JSON record of the toolchain that produced this build",
    )
    parser.add_argument(
        "--jobs", "-j",
        type=int,
        default=None,
        metavar="N",
        help="Maximum parallel compiler jobs (default: auto; 1 = legacy serial build)",
    )
    static_analysis.add_arguments(parser)
    security_gates.add_arguments(parser)
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
    parser.add_argument(
        "--inspect-arm64-windows-driver",
        metavar="DIR",
        default=None,
        help="Verify (no arm64 hardware needed) that an extracted Windows-on-Arm "
             "NVIDIA driver dir ships arm64 nvapia64.dll / nvml.dll and our entry "
             "points; extract the setup .exe with 7-Zip first",
    )
    args = parser.parse_args()
    if args.jobs is not None and args.jobs < 1:
        parser.error("--jobs must be >= 1")
    return args


def run_clang_tidy(write_baseline=False):
    return static_analysis.run_clang_tidy(
        _gate_ctx(), write_baseline=write_baseline)


def _needs_zig(target, arch):
    """Zig builds every Linux target, and links Windows ARM64.

    The ARM64 half is easy to miss: a Windows-only build still shells out to
    Zig for aarch64 (llvm-mingw's aarch64 linker hits a misaligned ldr/str
    bug), so a run that fetched llvm-mingw alone would fail at link time with
    no compiler to blame.  It only ever worked because an earlier --target all
    had already left zig/ on disk.
    """
    if target in ("linux", "all"):
        return True
    return target == "windows" and "arm64" in requested_arches(arch)


def _toolchain_components(target, arch="all"):
    """The pinned components a given target needs, for verification/reporting."""
    components = []
    if target in ("windows", "all"):
        components.append(("llvm-mingw", LLVM_MINGW_VERSION, LLVM_MINGW_DIR))
        components.append(("7zip", toolchain.SEVEN_ZIP_VERSION,
                           toolchain.seven_zip_dir(SCRIPT_DIR)))
    if _needs_zig(target, arch):
        components.append(("zig", ZIG_VERSION, ZIG_DIR))
    return components


def fetch_toolchain(target, arch="all"):
    """Vendor every pinned archive this host needs into compilers/.

    This is the deliberate "go and get the pinned bytes" step, so it is the one
    path that still reaches the network when the build itself is restricted to
    local toolchains only.
    """
    print("=== Fetching pinned toolchain into compilers/ ===")
    for tool, version, _root in _toolchain_components(target, arch):
        print(f"{tool} {version}")
        toolchain.fetch_into_compilers(COMPILERS_DIR, tool, version)
    return 0


def ensure_toolchain(target, arch="all"):
    """Download and verify the toolchain(s) needed for the given target."""
    if target in ("windows", "all"):
        download_llvm_mingw()
        # 7-Zip writes the Windows release archives, so it shapes a published
        # artifact just as much as the compiler does.  It is pinned and vendored
        # with the compilers rather than installed from whatever version the
        # host distribution happens to ship that week.
        toolchain.ensure_seven_zip(SCRIPT_DIR, COMPILERS_DIR)
    if _needs_zig(target, arch):
        download_zig()


def check_toolchain_pinning(build_script):
    """Every tool that can shape a released artifact is pinned and vendored.

    build.py keeps its own copies of a few digests because the gates above
    assert on them by name; this is what stops those copies from drifting away
    from compilers/*/manifest.json, which is the real record.
    """
    host = toolchain.host_key()
    toolchain.check_pins(COMPILERS_DIR, {
        "ZIG_SHA256": ("zig", ZIG_VERSION, host, "archive", ZIG_SHA256),
        "ZIG_EXE_SHA256": ("zig", ZIG_VERSION, host, _ZIG_EXE_NAME, ZIG_EXE_SHA256),
        "LLVM_MINGW_SHA256": ("llvm-mingw", LLVM_MINGW_VERSION, host, "archive",
                              LLVM_MINGW_SHA256),
        "LLVM_MINGW_CLANG_SHA256": (
            "llvm-mingw", LLVM_MINGW_VERSION, host,
            "bin/clang++.exe" if sys.platform == "win32" else "bin/clang-target-wrapper.sh",
            LLVM_MINGW_CLANG_SHA256),
    })
    require_text(build_script, "COMPILERS_REPO_BASE",
                 "toolchain archives are served from this repository's own release")
    # Assembled at runtime so this guard cannot match its own definition.
    forbid_text(build_script, "mstorsjo/llvm-mingw" + "/releases/download",
                "no toolchain is fetched from a third-party release at build time")
    workflows = os.path.join(SCRIPT_DIR, ".github", "workflows")
    release_workflow = os.path.join(workflows, "release.yml")
    if os.path.exists(release_workflow):
        require_text(release_workflow, toolchain.LOCAL_ONLY_ENV,
                     "the attested release build may only use vendored toolchains")
        require_text(release_workflow, "--fetch-toolchain",
                     "the release build vendors the pinned toolchain before building")
        forbid_text(release_workflow, "actions/cache",
                    "the release build unpacks compilers from the pinned archive, never a cache")
        forbid_text(release_workflow, "p7zip",
                    "release packaging uses the pinned 7-Zip, not a distribution package")
    problems = toolchain.unpinned_actions(
        [release_workflow, os.path.join(workflows, "ci.yml")])
    if problems:
        print("Regression source check FAILED: GitHub Actions must be pinned to "
              "commit SHAs, not mutable tags")
        for problem in problems:
            print(f"  {problem}")
        sys.exit(1)


def main():
    args = parse_args()
    if args.inspect_aarch64_driver:
        sys.exit(driver_inspect.inspect_aarch64_driver(
            _gate_ctx(), args.inspect_aarch64_driver))
    if args.inspect_arm64_windows_driver:
        sys.exit(driver_inspect.inspect_arm64_windows_driver(
            _gate_ctx(), args.inspect_arm64_windows_driver))
    if args.check_cet is not None:
        # Each host's gate compiles its own probe: the PE path needs llvm-mingw,
        # the ELF path needs Zig.  Fetching the other one is pure waste.
        ensure_toolchain("linux" if sys.platform.startswith("linux") else "windows")
        sys.exit(check_cet_instrumentation(args.check_cet or None))
    print("=== Green Curve build ===")
    _target_oses = resolve_targets(args.target or "all")
    args.target = "all" if len(_target_oses) > 1 else _target_oses[0]
    if args.fetch_toolchain:
        sys.exit(fetch_toolchain(args.target, args.arch))
    ensure_toolchain(args.target, args.arch)
    if args.verify_toolchain or args.toolchain_manifest:
        toolchain.report(COMPILERS_DIR,
                         _toolchain_components(args.target, args.arch),
                         args.toolchain_manifest)
        if args.verify_toolchain:
            sys.exit(0)
    jobs = build_scheduler.resolve_jobs(args.jobs)
    limiter = build_scheduler.JobLimiter(jobs)
    print(f"Using {jobs} parallel build job(s)")
    real_build = (not args.lsp and not args.check and not args.test
                  and not args.fuzz and not args.tidy
                  and not args.tidy_baseline)
    configure_build_number(real_build)

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
        if args.tidy or args.tidy_baseline:
            sys.exit(run_clang_tidy(write_baseline=args.tidy_baseline))
        if args.test:
            # Always run with UBSan by default (F-01-001).
            # --sanitizer is accepted for backward compatibility but is now the default behavior.
            test_extra_flags = list(SANITIZER_FLAGS)
            if args.asan:
                test_extra_flags.append("-fsanitize=address")
                test_extra_flags.append("-g")
            run_regression_tests(extra_flags=test_extra_flags)
            if not args.check and not args.fuzz:
                print("=== Done ===")
                return
        if args.fuzz:
            run_fuzz_targets(runs=args.fuzz_runs, target_filter=args.fuzz_target)
            if not args.check:
                print("=== Done ===")
                return
        if args.check:
            run_check_builds(args.target, args.arch, generate_lsp=not args.sanitizer,
                             jobs=jobs, limiter=limiter)
            print("=== Done ===")
            return
        generate_lsp_files()
        oses = _target_oses
        arches = ["x64", "arm64"] if args.arch == "all" else [args.arch]
        built = []  # (os_name, arch, [binary_path, ...])

        def fresh_payload(os_name, arch):
            # Wipe + recreate the target's isolated folder so each build is clean
            # and no two targets ever share an output or temp path.
            payload = target_payload_dir(os_name, arch)
            shutil.rmtree(os.path.dirname(payload), ignore_errors=True)
            os.makedirs(payload, exist_ok=True)
            return payload

        if "windows" in oses:
            # Shared resources: generate once before any parallel worker touches them.
            generate_icon()
            compile_resources()

        specs = []
        for arch in arches:
            if "windows" in oses:
                payload = fresh_payload("windows", arch)
                gui = os.path.join(payload, "greencurve.exe")
                svc = os.path.join(payload, "greencurve-service.exe")
                built.append(("windows", arch, [gui, svc]))
                specs.append(lambda gui=gui, arch=arch: compile_windows_binary(
                    output_path=gui, temp_output=gui + ".new", backup_path=gui + ".bak",
                    arch=arch, jobs=jobs, limiter=limiter))
                specs.append(lambda svc=svc, arch=arch: compile_windows_service_binary(
                    output_path=svc, temp_output=svc + ".new", backup_path=svc + ".bak",
                    arch=arch, jobs=jobs, limiter=limiter))
            if "linux" in oses:
                payload = fresh_payload("linux", arch)
                out = os.path.join(payload, "greencurve")
                built.append(("linux", arch, [out]))
                specs.append(lambda out=out, arch=arch: compile_linux_binary(
                    output_path=out, temp_output=out + ".new", backup_path=out + ".bak",
                    arch=arch, jobs=jobs, limiter=limiter))
        if specs:
            build_scheduler.run_parallel(specs, jobs)
        if not args.no_package and built:
            # A missing 7-Zip is not a build failure: the binaries are already
            # built and verified, so packaging degrades to the --no-package end
            # state with a loud warning instead of a traceback.  It can only
            # hold up Windows; the Linux tarball needs no external archiver.
            seven = find_seven_zip()
            packaged = [entry for entry in built if seven or entry[0] != "windows"]
            skipped = [entry for entry in built if entry not in packaged]
            if packaged:
                print("--- Packaging release archives ---")
            for os_name, arch, binaries in packaged:
                package_release_archive(os_name, arch, binaries, seven=seven)
            if skipped:
                report_packaging_skipped(skipped)
        print("=== Done ===")
    finally:
        COMMON_FLAGS[:] = _original_common_flags


if __name__ == "__main__":
    main()
