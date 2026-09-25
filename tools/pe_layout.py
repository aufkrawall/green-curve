# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Post-link PE debug-layout normalization for release artifacts.

LLD-MinGW places the CodeView RSDS record in a dedicated ".buildid" section.
That section name is a known antivirus/CAPE "obfuscation" tag, and no linker
flag accepted by zig c++ 0.13.0 can suppress it, so the release pipeline
normalizes every shipped PE after linking (before stamp_pe_checksum, which
must stay the LAST byte edit).

Rung 2 (this baseline): rewrite the 8-byte section-header NAME field from
".buildid" to ".rdata".  The name is a duplicate of the .rdata name, which is
tolerated by the loader; every offset, RVA and byte of section data stays
exactly where the linker put it, so there is zero relocation/checksum-order
risk.  A file without ".buildid" is left untouched.
"""

import struct


_BUILDID_NAME = b".buildid"
# Exactly 8 bytes (NUL-padded): replacing an 8-byte field with fewer bytes
# would shift the bytearray instead of rewriting the field.
_RDATA_NAME = b".rdata\0\0"
_SECTION_HEADER_SIZE = 40


def _section_table_of(data):
    """Return (pe_offset, section_table_offset, section_count) or raise."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("cannot normalize debug layout of a non-PE artifact")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("cannot normalize debug layout without a PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    if not count or table + count * _SECTION_HEADER_SIZE > len(data):
        raise RuntimeError("cannot normalize debug layout of a truncated section table")
    return pe, table, count


def normalize_pe_debug_layout(pe_path):
    """Rewrite ".buildid" section names to ".rdata" in-place; return the count.

    No-op (returns 0) when the image carries no ".buildid" section.  Fails
    loudly on anything that is not a structurally readable PE.
    """
    with open(pe_path, "r+b") as handle:
        data = bytearray(handle.read())
        _, table, count = _section_table_of(data)
        renamed = 0
        for index in range(count):
            header = table + index * _SECTION_HEADER_SIZE
            if data[header:header + 8] == _BUILDID_NAME:
                data[header:header + 8] = _RDATA_NAME
                renamed += 1
        if renamed > 1:
            raise RuntimeError(f"{pe_path}: PE carries {renamed} .buildid sections, expected at most 1")
        if renamed:
            handle.seek(0)
            handle.write(data)
            handle.truncate()
    return renamed


def run_self_tests():
    """Deterministic checks for the section-name rewrite."""
    failures = []

    def expect(condition, label):
        if not condition:
            failures.append(label)

    def synthetic_pe(names):
        """Minimal PE image with one section header per given 8-byte name."""
        data = bytearray(0x400)
        data[0:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", data, 0x86, len(names))
        struct.pack_into("<H", data, 0x94, 0xF0)   # SizeOfOptionalHeader
        struct.pack_into("<H", data, 0x98, 0x20B)  # PE32+
        table = 0x98 + 0xF0
        for index, name in enumerate(names):
            header = table + index * _SECTION_HEADER_SIZE
            data[header:header + 8] = name
            struct.pack_into("<IIII", data, header + 8,
                             0x200, 0x1000 * (index + 1), 0x200, 0x200 * (index + 1))
        return bytes(data)

    import tempfile, os
    with tempfile.TemporaryDirectory() as work:
        def normalize_bytes(payload):
            path = os.path.join(work, "fixture.exe")
            with open(path, "wb") as handle:
                handle.write(payload)
            renamed = normalize_pe_debug_layout(path)
            with open(path, "rb") as handle:
                return renamed, handle.read()

        # 1. The rename happens and everything else is byte-identical.
        fixture = synthetic_pe([b".text\0\0\0", b".rdata\0\0", b".buildid", b".data\0\0\0"])
        renamed, result = normalize_bytes(fixture)
        expect(renamed == 1, "a .buildid section is renamed exactly once")
        expect(result[0x188 + 2 * 40:0x188 + 2 * 40 + 8] == b".rdata\0\0",
               "the .buildid header name becomes .rdata")
        expect(len(result) == len(fixture), "the file length is unchanged")
        expect(result[0x188:0x188 + 40] == fixture[0x188:0x188 + 40],
               "the .text header is byte-identical")
        expect(result[0x188 + 3 * 40:] == fixture[0x188 + 3 * 40:],
               "the .data header and all section bytes are byte-identical")

        # 2. An image without .buildid is a strict no-op.
        clean = synthetic_pe([b".text\0\0\0", b".rdata\0\0", b".data\0\0\0"])
        renamed, result = normalize_bytes(clean)
        expect(renamed == 0, "an image without .buildid reports no rename")
        expect(result == clean, "an image without .buildid is byte-identical")

        # 3. Two .buildid sections fail loudly instead of being renamed.
        doubled = synthetic_pe([b".buildid", b".buildid"])
        try:
            normalize_bytes(doubled)
            failures.append("two .buildid sections were accepted")
        except RuntimeError:
            pass

        # 4. Truncated and garbage files fail loudly.
        for label, payload in (("truncated", fixture[:0x100]), ("garbage", b"\0" * 0x400)):
            try:
                normalize_bytes(payload)
                failures.append(f"a {label} file was accepted")
            except RuntimeError:
                pass

    if failures:
        raise RuntimeError("pe_layout self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_layout self-tests passed")
