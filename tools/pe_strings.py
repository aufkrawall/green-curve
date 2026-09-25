# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Forbidden-string scan over shipped Windows PE bytes.

The per-artifact import bans in tools/pe_verify.py cover the import table only;
antivirus classifiers also score plain strings, and a name that appears in an
image without being imported (a log literal, a resource, a delay-load stub, or
a dynamic resolution the project forbids anyway) is the same signal.  This
scan rejects the banned family names ANYWHERE in the raw image bytes, in both
ASCII and UTF-16LE, case-insensitively.

Matching is plain substring on purpose: the ban lists name stems whose A/W/Ex
spellings (SetWindowsHookExW, CreateRemoteThreadEx) contain the stem, and the
one documented prefix ban (SystemFunction0...) needs prefix semantics anyway.
SetWinEventHook is deliberately NOT banned anywhere: the GUI's auto-profiles
legitimately watch foreground changes with it.

Pure and read-only: never modifies the bytes it verifies.  tools/ dependency
convention: imports stdlib only, never build.py.
"""

# Every shipped image: the injection family (one leg of the
# alloc/protect/thread loader triad), anti-debug, download/exec, and the
# keyboard-hook family.  Service and setup legitimately keep their WinHTTP /
# token APIs and the GUI keeps SetWinEventHook, so those are not here.
_COMMON_FORBIDDEN_STRINGS = {
    # process injection family
    "VirtualAllocEx", "WriteProcessMemory", "CreateRemoteThread",
    "NtCreateThreadEx", "NtUnmapViewOfSection", "QueueUserAPC",
    # anti-debug
    "IsDebuggerPresent", "CheckRemoteDebuggerPresent", "NtQueryInformationProcess",
    # download/exec
    "URLDownloadToFile", "WinExec", "urlmon.dll",
    # keyboard-hook family
    "SetWindowsHookEx",
    # undocumented SystemFunction0XX aliases (SystemFunction036 is a
    # RtlGenRandom spelling); banned as a prefix
    "SystemFunction0",
}

# Token-relay names: the setup needs them to relaunch the GUI unelevated and
# the service keeps its signed-updater surface, but neither the GUI nor the
# uninstaller may carry them as text (the GUI import ban and the uninstaller
# surface gate already reject the imports).
_TOKEN_RELAY_STRINGS = {"WTSQueryUserToken", "CreateProcessWithTokenW"}

_FORBIDDEN_BY_ARTIFACT = {
    "gui": _TOKEN_RELAY_STRINGS,
    "service": frozenset(),
    "setup": frozenset(),
    "uninstaller": _TOKEN_RELAY_STRINGS,
}


def artifact_kind(original_filename):
    """Classify a shipped Windows artifact by its VERSIONINFO OriginalFilename,
    the same identity tools/pe_verify.py gates."""
    name = (original_filename or "").lower()
    if name == "greencurve.exe":
        return "gui"
    if name == "greencurve-service.exe":
        return "service"
    if "uninstall" in name:
        return "uninstaller"
    if "setup" in name:
        return "setup"
    raise RuntimeError(f"unknown Windows artifact identity {original_filename!r}")


def forbidden_strings(original_filename):
    """The full ban set for one shipped artifact."""
    return _COMMON_FORBIDDEN_STRINGS | set(
        _FORBIDDEN_BY_ARTIFACT[artifact_kind(original_filename)])


def find_forbidden_strings(data, original_filename):
    """Names of banned strings present in `data`, ASCII or UTF-16LE."""
    lowered = bytes(data).lower()
    hits = []
    for name in sorted(forbidden_strings(original_filename)):
        needle = name.lower()
        if needle.encode("ascii") in lowered or needle.encode("utf-16le") in lowered:
            hits.append(name)
    return hits


def verify_no_forbidden_strings(data, label, original_filename):
    hits = find_forbidden_strings(data, original_filename)
    if hits:
        raise RuntimeError(f"{label}: image contains banned strings "
                           f"({', '.join(hits)})")


def run_self_tests():
    """Deterministic checks for the forbidden-string scan."""
    failures = []

    def expect(condition, label):
        if not condition:
            failures.append(label)

    clean = b"ordinary image bytes CreateFileW greencurve"
    for kind_name in ("greencurve.exe", "greencurve-service.exe",
                      "greencurve-0.27.0-windows-x64-setup.exe",
                      "greencurve-uninstall.exe"):
        expect(find_forbidden_strings(clean, kind_name) == [],
               f"a clean image was flagged for {kind_name}")
        expect(find_forbidden_strings(b"SetWinEventHook", kind_name) == [],
               f"SetWinEventHook was flagged for {kind_name}")
    for name in sorted(_COMMON_FORBIDDEN_STRINGS):
        for encoding in ("ascii", "utf-16le"):
            fixture = b"prefix" + name.encode(encoding) + b"suffix"
            expect(find_forbidden_strings(fixture, "greencurve.exe") == [name],
                   f"{name} in {encoding} was not flagged")
    # Substring matching covers the A/W/Ex family spellings and the
    # undocumented-alias prefix ban.
    for spelling in ("CreateRemoteThreadEx", "SetWindowsHookExA",
                     "SetWindowsHookExW", "SystemFunction036"):
        expect(find_forbidden_strings(spelling.encode("ascii"),
                                      "greencurve.exe") != [],
               f"{spelling} was not flagged")
    expect(find_forbidden_strings(b"winexec", "greencurve.exe") == ["WinExec"],
           "the scan is case-insensitive")
    # Token-relay names are banned only where the feature cannot exist.
    for kind_name in ("greencurve.exe", "greencurve-uninstall.exe"):
        expect(find_forbidden_strings(b"WTSQueryUserToken", kind_name)
               == ["WTSQueryUserToken"],
               f"the token-relay string was not flagged for {kind_name}")
    for kind_name in ("greencurve-service.exe",
                      "greencurve-0.27.0-windows-x64-setup.exe"):
        expect(find_forbidden_strings(b"WTSQueryUserToken", kind_name) == [],
               f"the token-relay string was flagged for {kind_name}")
    try:
        verify_no_forbidden_strings(b"VirtualAllocEx", "fixture", "greencurve.exe")
        failures.append("verify_no_forbidden_strings accepted a banned string")
    except RuntimeError:
        pass
    try:
        verify_no_forbidden_strings(clean, "fixture", "greencurve-uninstall.exe")
    except RuntimeError as error:
        failures.append(f"a clean image was rejected: {error}")
    try:
        artifact_kind("mystery.bin")
        failures.append("an unknown artifact identity was accepted")
    except RuntimeError:
        pass

    if failures:
        raise RuntimeError("pe_strings self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_strings self-tests passed")
