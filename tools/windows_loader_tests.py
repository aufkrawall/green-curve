# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Native loader acceptance using a harmless sibling import fixture."""
import os
import subprocess


def _run(command, cwd, expected=0):
    process = subprocess.Popen(
        command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        output, _ = process.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       capture_output=True, timeout=15, check=False)
        process.communicate(timeout=15)
        raise RuntimeError("Windows loader fixture exceeded its execution bound") from None
    if process.returncode != expected:
        raise RuntimeError(f"Windows loader fixture returned {process.returncode}, "
                           f"expected {expected}: {output[-4000:]}")


def run(ctx, temporary):
    directory = os.path.join(temporary, "loader")
    os.makedirs(directory, exist_ok=True)
    source = os.path.join(ctx.SCRIPT_DIR, "tests", "windows_loader_fixture.cpp")
    base = [ctx.LLVM_MINGW_CLANG, "-std=c++17", "-static", "-fno-exceptions",
            "-fno-rtti", source]
    _run(base + ["-DGC_LOADER_FIXTURE_DLL=1", "-shared", "-o", "uxtheme.dll"], directory)
    for hardened in (False, True):
        executable = os.path.join(directory, "hardened.exe" if hardened else "baseline.exe")
        flags = ["-Wl,--dependent-load-flag=0x800"] if hardened else []
        _run(base + flags + ["-luxtheme", "-o", executable], directory)
        _run([executable], directory, 0 if hardened else 37)
    print("Windows static-loader fixture passed (baseline and restricted search)")
