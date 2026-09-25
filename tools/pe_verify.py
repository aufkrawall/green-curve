# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""PE/COFF release-artifact verification and CodeView sanitization.

Moved out of build.py (one-way dependency, same pattern as the other tools/
modules) and extended for the two Windows toolchains:

- llvm-mingw: OS CFG is emulated by the cfg_glue.cpp shim; the load config
  carries a Guard CF function table but no CET_COMPAT opt-in and no /GS cookie.
- clang-cl (MSVC ABI): real kernel-enforced CFG, a /GS security cookie, and —
  on x64 — the /cetcompat shadow-stack opt-in bit in the debug directory.
  ARM64 additionally gains CFG metadata (GFIDS) the Zig build never had.

The gates are toolchain-aware so a flag can never silently stop applying on
either pipeline.
"""

import struct

import pe_resources  # one-way tools/ dependency: it never imports build.py
import pe_strings  # ditto; owns the banned-string scan over raw image bytes

# PE layout primitives are shared with pe_resources' resource/CodeView
# parsing; re-exported here so the call sites below stay unchanged.
from pe_resources import _pe_data_directory, _rva_to_offset, _sections_of


# IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT (debug directory, type 20).
_CET_COMPAT_BIT = 0x0001
_EX_DLLCHARACTERISTICS_TYPE = 20


def _pe_ascii_string(data, offset):
    if offset is None or offset < 0 or offset >= len(data):
        raise RuntimeError("PE import string is out of bounds")
    end = data.find(b"\0", offset, min(len(data), offset + 512))
    if end < 0:
        raise RuntimeError("unterminated PE import string")
    return data[offset:end].decode("ascii", errors="replace")


def pe_imports(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    if pe + 24 + 112 + 16 * 8 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE optional header")
    sections = _sections_of(data, pe, optional)
    import_rva, import_size = _pe_data_directory(data, 1)
    if not import_rva or not import_size:
        return []
    import_offset = _rva_to_offset(sections, import_rva)
    if import_offset is None:
        raise RuntimeError("PE import directory is not backed by file data")
    imports = []
    max_descriptors = max(1, (import_size + 19) // 20)
    terminated = False
    for index in range(max_descriptors):
        descriptor = import_offset + index * 20
        if descriptor + 20 > len(data):
            raise RuntimeError("truncated PE import descriptor")
        original_first, _, _, name_rva, first_thunk = struct.unpack_from(
            "<IIIII", data, descriptor)
        if not any((original_first, name_rva, first_thunk)):
            terminated = True
            break
        name_offset = _rva_to_offset(sections, name_rva)
        dll = _pe_ascii_string(data, name_offset).lower()
        thunk_rva = original_first or first_thunk
        thunk_offset = _rva_to_offset(sections, thunk_rva)
        if thunk_offset is None:
            raise RuntimeError("PE import thunk is not backed by file data")
        functions = set()
        for thunk_index in range(4096):
            entry = thunk_offset + thunk_index * 8
            if entry + 8 > len(data):
                raise RuntimeError("truncated PE import thunk")
            value = struct.unpack_from("<Q", data, entry)[0]
            if not value:
                break
            if value & (1 << 63):
                functions.add(f"ordinal:{value & 0xFFFF}")
            else:
                hint_name_offset = _rva_to_offset(
                    sections, value & 0x7FFFFFFFFFFFFFFF)
                if hint_name_offset is None or hint_name_offset + 2 > len(data):
                    raise RuntimeError("PE import name is not backed by file data")
                functions.add(_pe_ascii_string(data, hint_name_offset + 2))
        else:
            raise RuntimeError("PE import thunk table is too long")
        imports.append((dll, functions))
    if not terminated:
        raise RuntimeError("PE import descriptor table is not terminated")
    return imports


def pe_section_names(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    if table + count * 40 > len(data):
        raise RuntimeError("truncated PE section table")
    return [data[table + i * 40:table + i * 40 + 8].rstrip(b"\0").decode("ascii", "replace")
            for i in range(count)]


def verify_no_buildid_section(data, label):
    """LLD-MinGW puts the RSDS debug directory in a ".buildid" section unless
    the link merges it into .rdata (WINDOWS_FLAGS).  Gated for every Windows
    PE whatever the architecture: only MinGW-style toolchains emit that name
    (MSVC-style images never carry it), and the "obfuscation" tags that fire
    on it do not single out x64."""
    if ".buildid" in pe_section_names(data):
        raise RuntimeError(f"{label}: .buildid section was not merged into .rdata")


_RT_GROUP_ICON = 14


def pe_resource_ids(data, type_id):
    """Integer IDs of the resources of one type (names are reported as None)."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = _sections_of(data, pe, pe + 24)
    rsrc_rva, rsrc_size = _pe_data_directory(data, 2)
    if not rsrc_rva or not rsrc_size:
        return []
    root = _rva_to_offset(sections, rsrc_rva)
    if root is None:
        raise RuntimeError("PE resource directory is not backed by file data")

    def entries(offset):
        if offset + 16 > len(data):
            raise RuntimeError("truncated PE resource directory")
        named, numbered = struct.unpack_from("<HH", data, offset + 12)
        if offset + 16 + (named + numbered) * 8 > len(data):
            raise RuntimeError("truncated PE resource directory entries")
        return [struct.unpack_from("<II", data, offset + 16 + i * 8)
                for i in range(named + numbered)]

    for name, target in entries(root):
        if name & 0x80000000 or name != type_id:
            continue
        if not target & 0x80000000:
            raise RuntimeError("PE resource type entry is not a directory")
        return [None if child & 0x80000000 else child
                for child, _ in entries(root + (target & 0x7FFFFFFF))]
    return []


def verify_service_resources(data, label):
    """The service has no window; only the GUI loads the tray icons 111-115."""
    groups = pe_resource_ids(data, _RT_GROUP_ICON)
    if groups != [101]:
        raise RuntimeError(f"{label}: service icon groups are {groups!r}, expected only [101]")


# Windows exports A/W/Ex/ExW spellings of one API (SetWindowsHookExW,
# CreateRemoteThreadEx), while the ban lists name the stem.  A ban therefore
# matches its exact name or one of those suffixed spellings -- deliberately NOT
# a raw prefix match: a ban on CreateProcessW must not catch the legitimate
# CreateProcessAsUserW / CreateProcessWithTokenW.
_IMPORT_NAME_SUFFIXES = ("", "a", "w", "ex", "exw")


def _import_name_matches(name, banned):
    lowered = name.lower()
    stem = banned.lower()
    return any(lowered == stem + suffix for suffix in _IMPORT_NAME_SUFFIXES)


def verify_pe_import_surface(data, label, required_dlls=(), forbidden_dlls=(),
                             required_functions=(), forbidden_functions=(),
                             reject_exports=False):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    export_rva, export_size = _pe_data_directory(data, 0)
    if reject_exports and (export_rva or export_size):
        raise RuntimeError(f"{label}: PE exports are not allowed")
    delay_rva, delay_size = _pe_data_directory(data, 13)
    if delay_rva or delay_size:
        raise RuntimeError(f"{label}: delay imports are not allowed")
    imports = pe_imports(data)
    libraries = {dll for dll, _ in imports}
    functions = {function for _, names in imports for function in names}
    required_libraries = {name.lower() for name in required_dlls}
    forbidden_libraries = {name.lower() for name in forbidden_dlls}
    required_names = {name.lower() for name in required_functions}
    forbidden_names = {name.lower() for name in forbidden_functions}
    missing_libraries = sorted(required_libraries - libraries)
    present_forbidden = sorted(forbidden_libraries & libraries)
    missing_functions = sorted(required_names - {name.lower() for name in functions})
    present_forbidden_functions = sorted(
        name for name in functions
        if any(_import_name_matches(name, banned) for banned in forbidden_names))
    if missing_libraries or present_forbidden or missing_functions or present_forbidden_functions:
        details = []
        if missing_libraries:
            details.append("missing DLLs " + ",".join(missing_libraries))
        if present_forbidden:
            details.append("forbidden DLLs " + ",".join(present_forbidden))
        if missing_functions:
            details.append("missing functions " + ",".join(missing_functions))
        if present_forbidden_functions:
            details.append("forbidden functions " + ",".join(present_forbidden_functions))
        raise RuntimeError(f"{label}: PE import surface mismatch ({'; '.join(details)})")


def _contains_pe_text(data, value):
    return (value.encode("ascii") in data or
            value.encode("utf-16le") in data)


def verify_setup_has_no_decompressor(data, label):
    """The shipped setup stub reads only its stored, self-contained payload."""
    for name in ("cabinet.dll", "CreateDecompressor", "CloseDecompressor"):
        if _contains_pe_text(data, name):
            raise RuntimeError(f"{label}: setup contains unused decompressor surface ({name})")


_RT_MANIFEST = 24


def verify_windows_manifest_identity(data, label, original_filename):
    name = (original_filename or "").lower()
    # comctl32_v6: True = the manifest must carry the v6 common-controls SxS
    # dependency, False = it must not, None = not gated (the GUI template may
    # legitimately adopt it later).
    if name == "greencurve.exe":
        assembly_name = "GreenCurve"
        description = "Green Curve"
        elevation, comctl32_v6 = "asInvoker", None
    elif name == "greencurve-service.exe":
        assembly_name = "GreenCurveService"
        description = "Green Curve background service"
        elevation, comctl32_v6 = "asInvoker", False
    elif "uninstall" in name:
        assembly_name = "GreenCurveUninstall"
        description = "Green Curve uninstaller"
        elevation, comctl32_v6 = "requireAdministrator", True
    elif "setup" in name:
        assembly_name = "GreenCurveSetup"
        description = "Green Curve setup"
        elevation, comctl32_v6 = "requireAdministrator", True
    else:
        raise RuntimeError(f"{label}: unknown Windows artifact identity {original_filename!r}")
    # These whole-file claims remain as additional assertions; elevation and
    # the comctl32 v6 SxS token below come from the parsed RT_MANIFEST
    # resource, never from raw substrings (a setup file's payload would
    # satisfy those).
    for value, field in ((assembly_name, "assembly name"),
                         (description, "description"),
                         ('processorArchitecture="*"', "processor architecture")):
        if not _contains_pe_text(data, value):
            raise RuntimeError(f"{label}: manifest {field} is missing or wrong")
    if _contains_pe_text(data, 'processorArchitecture="amd64"'):
        raise RuntimeError(f"{label}: manifest claims amd64 for every architecture")
    manifests = pe_resources.pe_resource_data(data, _RT_MANIFEST)
    if len(manifests) != 1:
        raise RuntimeError(f"{label}: expected one embedded manifest, found {len(manifests)}")
    manifest = manifests[0][1]
    level = pe_resources.manifest_execution_level(manifest)
    if level != elevation:
        raise RuntimeError(f"{label}: manifest requestedExecutionLevel is {level!r}, "
                           f"expected {elevation!r}")
    has_comctl32_v6 = pe_resources.manifest_has_comctl32_v6(manifest)
    if comctl32_v6 is True and not has_comctl32_v6:
        raise RuntimeError(f"{label}: manifest lacks the comctl32 v6 SxS dependency")
    if comctl32_v6 is False and has_comctl32_v6:
        raise RuntimeError(f"{label}: manifest unexpectedly carries the "
                           "comctl32 v6 SxS dependency")


# The service never creates a window (g_app.hMainWnd is GUI-only), so window,
# tray, timer, and DPI imports in it are unreachable GUI code.  In a SYSTEM
# process they read as a spy/keylogger surface, and a child process with
# redirected output (CreatePipe) reads as a remote shell; the one former user,
# the nvidia-smi max-clock read, is an NVML query now.
SERVICE_FORBIDDEN_UI_FUNCTIONS = {
    "GetWindowTextA", "GetWindowTextW", "GetWindowTextLengthA", "SetWindowTextA",
    "SetWindowTextW", "GetFocus", "SendMessageA", "SendMessageW", "EnableWindow",
    "IsWindowEnabled", "IsWindowVisible", "RedrawWindow", "SetTimer", "KillTimer",
    "Shell_NotifyIconA", "Shell_NotifyIconW", "LoadIconA", "LoadIconW", "GetDC",
    "ReleaseDC", "SetProcessDPIAware", "CreateWindowExA", "CreateWindowExW",
    "CreatePipe", "VirtualAlloc",
}

# The mirror image: the GUI never becomes the service.  g_app.isServiceProcess
# is set only by service_main and the controlled-restart helper, both reached
# solely from the service binary's WinMain, so the service runtime was dead
# code in the GUI image (app_is_service_process() in source/app_shared.h now
# compiles it out).  An unprivileged desktop program that carries SCM entry
# points, client impersonation, and a pipe server it can never run reads as a
# backdoor to an ML classifier.
#
# CreateNamedPipeW is deliberately NOT here: the Zig ARM64 link runs without
# LTO (to preserve BTI/PAC through code generation) and does not drop it, while
# the other three builds do.  Creating a pipe is also far weaker evidence than
# accepting a client on one and impersonating it, which this set does cover.
GUI_FORBIDDEN_SERVICE_FUNCTIONS = {
    "SetServiceStatus", "StartServiceCtrlDispatcherW", "StartServiceCtrlDispatcherA",
    "RegisterServiceCtrlHandlerExW", "RegisterServiceCtrlHandlerW",
    "ImpersonateNamedPipeClient", "ImpersonateLoggedOnUser", "ConnectNamedPipe",
}


def verify_windows_binary_imports(data, label, original_filename,
                                  reject_exports=False):
    name = (original_filename or "").lower()
    common_forbidden_dlls = {"winhttp.dll", "cabinet.dll"}
    common_forbidden_functions = {
        "CreateRemoteThread", "WriteProcessMemory", "VirtualAllocEx",
        "NtCreateThreadEx", "QueueUserAPC", "SetWindowsHookEx",
        "CheckRemoteDebuggerPresent", "NtQueryInformationProcess",
        # Anti-debug family, banned in EVERY variant since 2026-09-25: the
        # MSVC-ABI link satisfies the static UCRT's fault-path reference
        # (__acrt_call_reportfault) locally through source/crt_debugger_shim.cpp
        # instead of importing the API, and the llvm-mingw CRT never imported
        # it.  See tools/pe_strings.py for the matching string-scan scope.
        "IsDebuggerPresent",
    }
    if name == "greencurve.exe":
        verify_pe_import_surface(
            data, label,
            required_dlls={"user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll"},
            forbidden_dlls=common_forbidden_dlls | {"wtsapi32.dll", "userenv.dll"},
            forbidden_functions=common_forbidden_functions | {
                "WTSQueryUserToken", "CreateProcessWithTokenW",
            } | GUI_FORBIDDEN_SERVICE_FUNCTIONS, reject_exports=reject_exports)
    elif name == "greencurve-service.exe":
        verify_service_resources(data, label)
        verify_pe_import_surface(
            data, label,
            required_dlls={"advapi32.dll", "winhttp.dll", "wtsapi32.dll", "userenv.dll"},
            forbidden_dlls={"cabinet.dll", "gdi32.dll", "comctl32.dll"},
            forbidden_functions=common_forbidden_functions | SERVICE_FORBIDDEN_UI_FUNCTIONS,
            reject_exports=reject_exports)
    elif "setup" in name:
        verify_setup_has_no_decompressor(data, label)
        verify_pe_import_surface(
            data, label,
            required_dlls={"user32.dll", "advapi32.dll", "shell32.dll", "wtsapi32.dll", "userenv.dll"},
            forbidden_dlls={"winhttp.dll", "cabinet.dll"},
            required_functions={"CreateProcessWithTokenW"},
            forbidden_functions=common_forbidden_functions,
            reject_exports=reject_exports)
    elif "uninstall" in name:
        verify_pe_import_surface(
            data, label,
            required_dlls={"user32.dll", "advapi32.dll", "shell32.dll"},
            forbidden_dlls=common_forbidden_dlls | {"wtsapi32.dll", "userenv.dll"},
            forbidden_functions=common_forbidden_functions | {
                "WTSQueryUserToken", "CreateProcessWithTokenW",
            }, reject_exports=reject_exports)
    else:
        raise RuntimeError(f"{label}: unknown Windows artifact identity {original_filename!r}")


def verify_windows_binary_metadata(data, label, original_filename, arch,
                                    windows_toolchain):
    verify_pe_hardening(data, arch, windows_toolchain)
    verify_pe_checksum(data, label)
    verify_version_identity(data, original_filename, label)
    verify_windows_manifest_identity(data, label, original_filename)
    verify_windows_binary_imports(
        data, label, original_filename,
        reject_exports=(windows_toolchain == "clang-cl"))
    # The import bans above cover the import table; the raw-byte scan also
    # catches the same family names as embedded text in any shipped image.
    pe_strings.verify_no_forbidden_strings(data, label, original_filename)
    # The RSDS record must name its PDB by bare basename: an absolute path
    # leaks the build workspace and means CodeView sanitization did not run.
    pe_resources.verify_codeview_pdb_basename(data, label)
    verify_no_buildid_section(data, label)


def verify_pe_hardening(data, arch, windows_toolchain="llvm-mingw"):
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
    sections = _sections_of(data, pe, optional)
    section_table = optional + struct.unpack_from("<H", data, pe + 20)[0]
    for index in range(len(sections)):
        characteristics = struct.unpack_from(
            "<I", data, section_table + index * 40 + 36)[0]
        if characteristics & 0xA0000000 == 0xA0000000:
            raise RuntimeError("PE has a writable/executable section")
    import_rva, import_size = struct.unpack_from("<II", data, optional + 112 + 8)
    if not import_rva or not import_size:
        raise RuntimeError("PE import dependency table is missing")
    load_rva, load_size = struct.unpack_from("<II", data, optional + 112 + 10 * 8)
    load_off = _rva_to_offset(sections, load_rva) if load_rva else None
    if arch == "x64":
        if load_off is None or load_size < 144 or load_off + 144 > len(data):
            raise RuntimeError("Windows x64 load-config/CFG table is missing")
        guard_table = struct.unpack_from("<Q", data, load_off + 128)[0]
        guard_count = struct.unpack_from("<Q", data, load_off + 136)[0]
        if not guard_table or not guard_count:
            raise RuntimeError("Windows x64 CFG function table is empty")
    elif arch == "arm64" and windows_toolchain == "clang-cl":
        # New with the MSVC-ABI build: real CFG metadata on ARM64.  The Zig
        # build never had it, so this gate is toolchain-conditional.
        if load_off is None or load_size < 144 or load_off + 144 > len(data):
            raise RuntimeError("Windows arm64 load-config/CFG table is missing")
        guard_table = struct.unpack_from("<Q", data, load_off + 128)[0]
        guard_count = struct.unpack_from("<Q", data, load_off + 136)[0]
        if not guard_table or not guard_count:
            raise RuntimeError("Windows arm64 CFG function table is empty")
    if arch == "x64" and windows_toolchain == "clang-cl":
        # The /cetcompat shadow-stack opt-in lives in the debug directory, not
        # the load config.  Require it so the flag can never silently stop
        # reaching the binary.
        ex_chars = debug_dir_ex_dllcharacteristics(data)
        if ex_chars is None or not ex_chars & _CET_COMPAT_BIT:
            raise RuntimeError(
                "Windows x64 CETCOMPAT shadow-stack opt-in missing "
                "(no EX_DLLCHARACTERISTICS CET_COMPAT debug entry); "
                "/cetcompat is not reaching the link")


def debug_dir_ex_dllcharacteristics(data):
    """Return the EX_DLLCHARACTERISTICS flags from the debug directory, or
    None when the entry is absent."""
    if len(data) < 0x100 or data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    sections = _sections_of(data, pe, optional)
    debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + 6 * 8)
    if not debug_rva:
        return None
    debug_offset = _rva_to_offset(sections, debug_rva)
    if debug_offset is None:
        return None
    for entry in range(debug_offset, debug_offset + debug_size, 28):
        if entry + 28 > len(data):
            break
        debug_type = struct.unpack_from("<I", data, entry + 12)[0]
        data_size = struct.unpack_from("<I", data, entry + 16)[0]
        data_pointer = struct.unpack_from("<I", data, entry + 24)[0]
        if debug_type != _EX_DLLCHARACTERISTICS_TYPE:
            continue
        if data_size < 2 or data_pointer + data_size > len(data):
            return None
        return struct.unpack_from("<H", data, data_pointer)[0]
    return None


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


# ---------------------------------------------------------------------------
# Image checksum and version identity
#
# Neither affects how Windows runs a user-mode image; both are metadata that
# antivirus heuristics and PE-feature classifiers score.  An image whose
# CheckSum is 0 (what LLD writes without /release) or whose VERSIONINFO names a
# different file than it ships as looks, respectively, "hand-patched" and
# "renamed" to those models.  Unsigned small projects have no reputation to
# outweigh such signals, so every shipped PE gets a correct checksum and its
# own identity, and the build gates both.
# ---------------------------------------------------------------------------

# Offset of OptionalHeader.CheckSum from the start of the optional header; the
# same for PE32 and PE32+.
_CHECKSUM_OPTIONAL_OFFSET = 64


def _checksum_field_offset(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("cannot checksum a non-PE artifact")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 + _CHECKSUM_OPTIONAL_OFFSET + 4 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("cannot checksum an image without a PE header")
    return pe + 24 + _CHECKSUM_OPTIONAL_OFFSET


def pe_image_checksum(data):
    """The CheckSumMappedFile / link.exe /RELEASE algorithm over the WHOLE file.

    16-bit one's-complement-style folding sum of every word except the
    CheckSum field itself, plus the file length.  The overlay is included, so a
    setup file must be stamped after its payload is appended."""
    field = _checksum_field_offset(data)
    padded = bytes(data) + (b"\0" if len(data) % 2 else b"")
    words = struct.unpack(f"<{len(padded) // 2}H", padded)
    total = sum(words) - words[field // 2] - words[field // 2 + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def stamp_pe_checksum(binary_path):
    """Write the correct image checksum into `binary_path`; returns it.

    Must be the LAST byte-level edit to the file: CodeView sanitization and the
    setup payload append both change the sum."""
    with open(binary_path, "r+b") as handle:
        data = bytearray(handle.read())
        checksum = pe_image_checksum(data)
        struct.pack_into("<I", data, _checksum_field_offset(data), checksum)
        handle.seek(0)
        handle.write(data)
    return checksum


def verify_pe_checksum(data, label):
    field = _checksum_field_offset(data)
    stored = struct.unpack_from("<I", data, field)[0]
    expected = pe_image_checksum(data)
    if stored != expected:
        raise RuntimeError(f"{label}: PE CheckSum is 0x{stored:08x}, expected 0x{expected:08x} "
                           "(stamp_pe_checksum must run after the last edit to the file)")


def version_string_value(data, key):
    """Return a StringFileInfo value (e.g. "OriginalFilename"), or None.

    Located by its UTF-16LE key rather than by walking the resource tree: a
    String entry is key, NUL, zero padding to a DWORD boundary, then the
    NUL-terminated value, and no value this project emits is empty."""
    needle = key.encode("utf-16le") + b"\0\0"
    at = data.find(needle)
    if at < 0:
        return None
    cursor = at + len(needle)
    while cursor + 1 < len(data) and data[cursor:cursor + 2] == b"\0\0":
        cursor += 2
    end = cursor
    while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
        end += 2
    try:
        return bytes(data[cursor:end]).decode("utf-16le")
    except UnicodeDecodeError:
        return None


def _version_translation(data):
    """The VarFileInfo Translation (language, charset) pair, or None.

    Located by its UTF-16LE key like the string values: a Var entry is key,
    NUL, DWORD padding, then the binary value (two little-endian WORDs)."""
    needle = "Translation".encode("utf-16le") + b"\0\0"
    at = data.find(needle)
    if at < 0:
        return None
    cursor = at + len(needle)
    while cursor + 1 < len(data) and data[cursor:cursor + 2] == b"\0\0":
        cursor += 2
    if cursor + 4 > len(data):
        return None
    return struct.unpack_from("<HH", data, cursor)


def verify_version_identity(data, expected_original_filename, label):
    """Every shipped PE names itself and its publisher."""
    original = version_string_value(data, "OriginalFilename")
    if original is None or original.lower() != expected_original_filename.lower():
        raise RuntimeError(f"{label}: VERSIONINFO OriginalFilename is {original!r}, "
                           f"expected {expected_original_filename!r}")
    for key in ("CompanyName", "Comments", "FileDescription", "FileVersion",
                "LegalCopyright", "ProductName", "ProductVersion", "InternalName"):
        if not version_string_value(data, key):
            raise RuntimeError(f"{label}: VERSIONINFO {key} is missing or empty")
    translation = _version_translation(data)
    if translation != (0x0409, 1200):
        raise RuntimeError(f"{label}: VERSIONINFO Translation is {translation!r}, "
                           "expected (0x0409, 1200)")
    block = f"{translation[0]:04X}{translation[1]:04X}"
    if block.encode("utf-16le") not in bytes(data):
        raise RuntimeError(f"{label}: VERSIONINFO StringFileInfo block {block} is missing "
                           "(inconsistent with the VarFileInfo Translation)")


def run_self_tests():
    """Delegate; tools/pe_verify_tests.py owns the fixtures and checks so this
    module stays focused on the gates (source size discipline)."""
    import pe_verify_tests  # function-level: that module imports this one
    pe_verify_tests.run_self_tests()
