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


# IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT (debug directory, type 20).
_CET_COMPAT_BIT = 0x0001
_EX_DLLCHARACTERISTICS_TYPE = 20


def _sections_of(data, pe, optional):
    number_of_sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    section_table = optional + optional_size
    sections = []
    for index in range(number_of_sections):
        section = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, section + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))
    return sections


def _rva_to_offset(sections, rva):
    for virtual_address, span, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + span:
            return raw_pointer + (rva - virtual_address)
    return None


def _pe_data_directory(data, index):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    if pe + 24 + 112 + (index + 1) * 8 > len(data) or \
            data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE data directory")
    return struct.unpack_from("<II", data, optional + 112 + index * 8)


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
        forbidden_names & {name.lower() for name in functions})
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


def verify_windows_manifest_identity(data, label, original_filename):
    name = (original_filename or "").lower()
    if name == "greencurve.exe":
        assembly_name = "GreenCurve"
        description = "Green Curve"
    elif name == "greencurve-service.exe":
        assembly_name = "GreenCurveService"
        description = "Green Curve background service"
    elif "uninstall" in name:
        assembly_name = "GreenCurveUninstall"
        description = "Green Curve uninstaller"
    elif "setup" in name:
        assembly_name = "GreenCurveSetup"
        description = "Green Curve setup"
    else:
        raise RuntimeError(f"{label}: unknown Windows artifact identity {original_filename!r}")
    for value, field in ((assembly_name, "assembly name"),
                         (description, "description"),
                         ('processorArchitecture="*"', "processor architecture")):
        if not _contains_pe_text(data, value):
            raise RuntimeError(f"{label}: manifest {field} is missing or wrong")
    if _contains_pe_text(data, 'processorArchitecture="amd64"'):
        raise RuntimeError(f"{label}: manifest claims amd64 for every architecture")


def verify_windows_binary_imports(data, label, original_filename,
                                  reject_exports=False):
    name = (original_filename or "").lower()
    common_forbidden_dlls = {"winhttp.dll", "cabinet.dll"}
    common_forbidden_functions = {
        "CreateRemoteThread", "WriteProcessMemory", "VirtualAllocEx",
        "NtCreateThreadEx", "QueueUserAPC", "SetWindowsHookEx",
    }
    if name == "greencurve.exe":
        verify_pe_import_surface(
            data, label,
            required_dlls={"user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll"},
            forbidden_dlls=common_forbidden_dlls | {"wtsapi32.dll", "userenv.dll"},
            forbidden_functions=common_forbidden_functions | {
                "WTSQueryUserToken", "CreateProcessWithTokenW",
            }, reject_exports=reject_exports)
    elif name == "greencurve-service.exe":
        verify_pe_import_surface(
            data, label,
            required_dlls={"advapi32.dll", "winhttp.dll", "wtsapi32.dll", "userenv.dll"},
            forbidden_dlls={"cabinet.dll"},
            forbidden_functions=common_forbidden_functions,
            reject_exports=reject_exports)
    elif "setup" in name:
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


def verify_version_identity(data, expected_original_filename, label):
    """Every shipped PE names itself and its publisher."""
    original = version_string_value(data, "OriginalFilename")
    if original is None or original.lower() != expected_original_filename.lower():
        raise RuntimeError(f"{label}: VERSIONINFO OriginalFilename is {original!r}, "
                           f"expected {expected_original_filename!r}")
    for key in ("CompanyName", "FileDescription", "ProductName", "InternalName"):
        if not version_string_value(data, key):
            raise RuntimeError(f"{label}: VERSIONINFO {key} is missing or empty")


def _synthetic_pe(length):
    """A deterministic byte pattern with just enough PE header to checksum."""
    data = bytearray((index * 37 + 11) & 0xFF for index in range(length))
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\x00\x00"
    struct.pack_into("<H", data, 0x80 + 4, 0x8664)   # Machine: AMD64
    struct.pack_into("<H", data, 0x80 + 20, 0xF0)    # SizeOfOptionalHeader
    struct.pack_into("<H", data, 0x80 + 24, 0x20B)   # PE32+ magic
    struct.pack_into("<I", data, 0x80 + 24 + _CHECKSUM_OPTIONAL_OFFSET, 0xDEADBEEF)
    return data


def _version_string_entry(key, value):
    """One StringFileInfo String: header, key, NUL, DWORD padding, value, NUL."""
    body = key.encode("utf-16le") + b"\0\0"
    header_and_key = 6 + len(body)
    body += b"\0" * ((4 - header_and_key % 4) % 4)
    body += value.encode("utf-16le") + b"\0\0"
    return struct.pack("<HHH", 6 + len(body), len(value) + 1, 1) + body


def _synthetic_import_pe():
    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    section = 0x98 + 0xF0
    data[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<I", data, section + 36, 0x60000020)
    struct.pack_into("<II", data, 0x98 + 112 + 8, 0x1000, 40)
    struct.pack_into("<IIIII", data, 0x200, 0x1050, 0, 0, 0x1030, 0x1050)
    data[0x230:0x23D] = b"KERNEL32.dll\0"
    struct.pack_into("<QQ", data, 0x250, 0x1070, 0)
    struct.pack_into("<H", data, 0x270, 0)
    data[0x272:0x27D] = b"CreateFileW\0"
    return bytes(data)


def run_self_tests():
    """Deterministic checks for the checksum and VERSIONINFO helpers."""
    failures = []

    def expect(condition, label):
        if not condition:
            failures.append(label)

    import_fixture = _synthetic_import_pe()
    try:
        imports = pe_imports(import_fixture)
        if imports != [("kernel32.dll", {"CreateFileW"})]:
            failures.append(f"PE import parser returned {imports!r}")
        verify_pe_import_surface(
            import_fixture, "import fixture",
            required_dlls={"KERNEL32.dll"}, required_functions={"CreateFileW"})
    except RuntimeError as error:
        failures.append(f"PE import fixture was rejected: {error}")
    try:
        verify_pe_import_surface(
            import_fixture, "import fixture",
            forbidden_functions={"CreateFileW"})
        failures.append("PE import surface accepted a forbidden function")
    except RuntimeError:
        pass

    # Reference values cross-checked against pefile.generate_checksum() and
    # against lld-link /release output when the helper was written.  The odd
    # length covers the zero-pad of the trailing byte.
    for length, reference in ((0x400, 0x34F3), (0x401, 0x34FF)):
        data = _synthetic_pe(length)
        expect(pe_image_checksum(data) == reference,
               f"checksum of the {length}-byte vector is 0x{pe_image_checksum(data):x}, "
               f"expected 0x{reference:x}")
        # The stored field must not feed into its own value.
        field = _checksum_field_offset(data)
        altered = bytearray(data)
        struct.pack_into("<I", altered, field, 0)
        expect(pe_image_checksum(altered) == reference, "the CheckSum field is excluded from the sum")
        # Zero (LLD's default) is what the gate exists to reject.
        try:
            verify_pe_checksum(altered, "vector")
            failures.append("verify_pe_checksum accepted CheckSum 0")
        except RuntimeError:
            pass
        struct.pack_into("<I", altered, field, reference)
        try:
            verify_pe_checksum(altered, "vector")
        except RuntimeError as error:
            failures.append(f"verify_pe_checksum rejected a correct checksum: {error}")
        # An appended overlay (the setup payload) changes the sum.
        expect(pe_image_checksum(bytes(altered) + b"payload") != reference,
               "the overlay is part of the checksum")

    identity = b"".join([
        b"\x00" * 7,  # misalignment the parser must not depend on
        _version_string_entry("CompanyName", "aufkrawall"),
        _version_string_entry("FileDescription", "Green Curve background service"),
        _version_string_entry("InternalName", "GreenCurveService"),
        _version_string_entry("OriginalFilename", "greencurve-service.exe"),
        _version_string_entry("ProductName", "Green Curve"),
    ])
    expect(version_string_value(identity, "OriginalFilename") == "greencurve-service.exe",
           "OriginalFilename is read back")
    expect(version_string_value(identity, "CompanyName") == "aufkrawall", "CompanyName is read back")
    expect(version_string_value(identity, "LegalTrademarks") is None, "an absent key reads as None")
    try:
        verify_version_identity(identity, "GREENCURVE-SERVICE.EXE", "service")
    except RuntimeError as error:
        failures.append(f"a matching identity was rejected: {error}")
    # The shape every service binary had before the split: it claimed to be the GUI.
    try:
        verify_version_identity(identity, "greencurve.exe", "gui")
        failures.append("a binary naming another file passed verify_version_identity")
    except RuntimeError:
        pass
    anonymous = identity.replace("CompanyName".encode("utf-16le"), "CompanyNamX".encode("utf-16le"))
    try:
        verify_version_identity(anonymous, "greencurve-service.exe", "service")
        failures.append("a binary without CompanyName passed verify_version_identity")
    except RuntimeError:
        pass

    if failures:
        raise RuntimeError("pe_verify self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_verify self-tests passed")
