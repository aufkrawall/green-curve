#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Host-correct clang-tidy ratchet for Green Curve.

Kept outside build.py so the main build orchestrator stays under its size
ratchet. The dependency is one-way: this module receives the live build module
as ``ctx`` and never imports it.
"""

import json
import os
import re
import shutil
import subprocess
import sys


# Checks worth failing a build over: real defect classes, not style. Broaden
# only after the existing baseline is cleared enough to keep the gate credible.
TIDY_CHECKS = ",".join([
    "-*",
    "bugprone-*",
    "cert-*",
    "clang-analyzer-*",
    "misc-*",
    "-bugprone-easily-swappable-parameters",
    "-bugprone-narrowing-conversions",
    "-misc-no-recursion",
    "-misc-non-private-member-variables-in-classes",
    "-misc-use-anonymous-namespace",
    "-misc-const-correctness",
    # The project deliberately uses amalgamated .cpp shards.
    "-misc-include-cleaner",
    "-bugprone-suspicious-include",
    "-misc-use-internal-linkage",
    "-clang-analyzer-optin.performance.Padding",
    "-bugprone-reserved-identifier",
    "-cert-dcl37-c",
    "-cert-dcl51-cpp",
    "-cert-dcl50-cpp",
    "-cert-err33-c",
])


def add_arguments(parser):
    parser.add_argument(
        "--tidy", action="store_true",
        help="Run clang-tidy and fail on findings outside the tracked baseline")
    parser.add_argument(
        "--tidy-baseline", action="store_true",
        help="Rewrite the clang-tidy baseline from the current findings")


def _finding_key(line, script_dir):
    """Reduce a diagnostic to path + check, excluding line-number churn."""
    match = re.match(
        r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+.*\[([^\]]+)\]\s*$",
        line.strip())
    if not match:
        return None
    path = os.path.relpath(match.group(1), script_dir).replace("\\", "/")
    return f"{path}\t{match.group(5)}"


def _load_baseline(path):
    if not os.path.exists(path):
        return set()
    with open(path, "r", encoding="utf-8") as handle:
        return {
            line.strip() for line in handle
            if line.strip() and not line.startswith("#")
        }


def _host_tidy(ctx):
    if sys.platform == "win32":
        tidy = os.path.join(
            os.path.dirname(ctx.LLVM_MINGW_CLANG), "clang-tidy.exe")
        if not os.path.exists(tidy):
            tidy = os.path.join(
                os.path.dirname(ctx.LLVM_MINGW_CLANG), "clang-tidy")
        return tidy
    return shutil.which("clang-tidy")


def _host_entries(ctx, entries):
    if sys.platform == "win32":
        return entries
    linux_sources = set(ctx.LINUX_SOURCE_FILES)
    linux_sources.add(
        os.path.join(ctx.SCRIPT_DIR, "tests", "regression_main.cpp"))
    return [
        entry for entry in entries
        if os.path.normpath(entry["file"]) in linux_sources
        and "-mguard=cf" not in entry.get("arguments", [])
    ]


def _windows_extra_args(ctx):
    if sys.platform != "win32":
        return []
    libcxx_include = os.path.join(
        os.path.dirname(os.path.dirname(ctx.LLVM_MINGW_CLANG)),
        "include", "c++", "v1")
    if not os.path.isdir(libcxx_include):
        raise RuntimeError(f"libc++ headers not found at {libcxx_include}")
    return ["--extra-arg=-isystem", f"--extra-arg={libcxx_include}"]


def _write_baseline(path, keys):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# clang-tidy baseline: path<TAB>check.\n")
        handle.write("# Regenerate with: python build.py --tidy-baseline\n")
        for key in sorted(keys):
            handle.write(key + "\n")


def run_clang_tidy(ctx, write_baseline=False):
    """Run the static-analysis ratchet over host-compatible compile commands."""
    tidy = _host_tidy(ctx)
    if not tidy or not os.path.exists(tidy):
        print("ERROR: clang-tidy is required for --tidy but was not found")
        return 1

    # Regenerate on this host. A Windows database contains PE-only flags, while
    # Linux needs Zig's libc++/glibc include roots.
    ctx.generate_lsp_files()
    database = os.path.join(ctx.SCRIPT_DIR, "compile_commands.json")
    with open(database, "r", encoding="utf-8") as handle:
        entries = _host_entries(ctx, json.load(handle))
    if not entries:
        print("ERROR: no host-compatible clang-tidy compile commands")
        return 1

    tidy_db = ctx.prepare_work_subdir("tidy-database")
    with open(os.path.join(tidy_db, "compile_commands.json"), "w",
              encoding="utf-8", newline="\n") as handle:
        json.dump(entries, handle, indent=2)
        handle.write("\n")
    try:
        extra_args = _windows_extra_args(ctx)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        ctx.cleanup_work_subdir(tidy_db)
        return 1

    sources = sorted({entry["file"] for entry in entries})
    print(f"Running clang-tidy over {len(sources)} translation unit(s)")
    findings = []
    execution_failures = []
    try:
        for source in sources:
            result = subprocess.run(
                [tidy, f"-checks={TIDY_CHECKS}", "-quiet",
                 f"-p={tidy_db}", *extra_args, source],
                cwd=ctx.SCRIPT_DIR, text=True, capture_output=True)
            combined = "\n".join(
                part for part in (result.stdout, result.stderr) if part)
            for line in combined.splitlines():
                key = _finding_key(line, ctx.SCRIPT_DIR)
                if key:
                    findings.append((key, line.strip()))
            if result.returncode != 0:
                tail = "\n".join(combined.splitlines()[-12:])
                execution_failures.append(
                    f"{os.path.relpath(source, ctx.SCRIPT_DIR)} "
                    f"(exit {result.returncode})\n{tail}")
    finally:
        ctx.cleanup_work_subdir(tidy_db)

    baseline_path = os.path.join(ctx.SCRIPT_DIR, "tidy-baseline.txt")
    baseline = _load_baseline(baseline_path)
    current = {key for key, _ in findings}
    source_paths = {
        os.path.relpath(path, ctx.SCRIPT_DIR).replace("\\", "/")
        for path in sources
    }
    if write_baseline:
        if sys.platform == "win32":
            merged = current
        else:
            preserved = {
                key for key in baseline
                if not key.split("\t", 1)[0].startswith("source/linux_")
                and key.split("\t", 1)[0] not in source_paths
            }
            merged = preserved | current
        _write_baseline(baseline_path, merged)
        print(f"Wrote {len(merged)} baseline entries to {baseline_path}")
        return 1 if execution_failures else 0

    by_check = {}
    for key, _ in findings:
        check = key.split("\t", 1)[1] if "\t" in key else key
        by_check[check] = by_check.get(check, 0) + 1
    if by_check:
        print("clang-tidy findings by check:")
        for check, count in sorted(
                by_check.items(), key=lambda item: (-item[1], item[0])):
            print(f"  {count:5d}  {check}")

    new_findings = [line for key, line in findings if key not in baseline]
    if new_findings:
        print(f"clang-tidy: {len(new_findings)} finding(s) not in the baseline:")
        for line in new_findings[:50]:
            print(f"  {line}")
        if len(new_findings) > 50:
            print(f"  ... and {len(new_findings) - 50} more")
    else:
        print(f"clang-tidy: no new findings ({len(baseline)} baselined)")

    stale = sorted(baseline - current)
    if sys.platform != "win32":
        stale = [
            key for key in stale
            if key.split("\t", 1)[0].startswith("source/linux_")
            or key.split("\t", 1)[0] in source_paths
        ]
    if stale:
        print(f"clang-tidy: {len(stale)} baselined finding(s) no longer "
              "reported; rerun --tidy-baseline to shrink the baseline")
    if execution_failures:
        print(f"clang-tidy: {len(execution_failures)} execution failure(s):")
        for failure in execution_failures:
            print(f"  {failure}")
    diagnostic_errors = [
        line for key, line in findings
        if key.endswith("\tclang-diagnostic-error")
    ]
    if diagnostic_errors:
        print("clang-tidy: compiler diagnostics are never baseline-eligible")
    return 1 if new_findings or execution_failures or diagnostic_errors else 0
