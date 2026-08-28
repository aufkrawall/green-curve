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
