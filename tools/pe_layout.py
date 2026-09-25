# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Post-link PE debug-layout normalization for release artifacts.

LLD-MinGW places the CodeView RSDS record in a dedicated ".buildid" section.
That section name is a known antivirus/CAPE "obfuscation" tag, and no linker
flag accepted by zig c++ 0.13.0 can suppress it, so the release pipeline
normalizes every shipped PE after linking and before stamp_pe_checksum
(which must stay the LAST byte edit).

Two rungs, chosen per input by normalize_pe_debug_layout():

- Rung 1 (preferred): merge_buildid_into_rdata() absorbs the .buildid raw
  bytes into .rdata verbatim, removes the section header (count -1) and
  repacks raw offsets on FileAlignment boundaries.  Every section RVA and
  SizeOfImage stay exactly as the linker wrote them (so .pdata, .reloc and
  all runtime pointers stay valid), the RSDS record bytes stay byte-identical,
  and the CodeView debug-directory entry is repointed at the record's new
  RVA inside .rdata's new virtual range.  Applied only when every safety
  assertion holds; the merge moves raw bytes verbatim so no content is
  discarded or overwritten (the .buildid tail lands after .rdata's raw end,
  which also promotes .rdata's alignment slack into its virtual size).

- Rung 2 (fallback): rewrite the 8-byte section-header NAME field from
  ".buildid" to ".rdata" in place.  The name is a duplicate of the .rdata
  name, which the loader tolerates; every offset, RVA and byte stays exactly
  where the linker put it, so there is zero relocation risk.  This is also
  byte-count neutral, which matters for images that carry a self-referential
  overlay (the setup payload footer records the stub length).

A file without ".buildid" is left untouched apart from the RSDS name
sanitization every call performs.
"""

import os
import struct

from pe_verify import sanitize_pe_codeview_path


_BUILDID_NAME = b".buildid"
# Exactly 8 bytes (NUL-padded): replacing an 8-byte field with fewer bytes
# would shift the bytearray instead of rewriting the field.
_RDATA_NAME = b".rdata\0\0"
_SECTION_HEADER_SIZE = 40
_DEBUG_DIRECTORY_INDEX = 6
_EXCEPTION_DIRECTORY_INDEX = 3
_RELOC_DIRECTORY_INDEX = 5
_DEBUG_ENTRY_SIZE = 28
_DEBUG_TYPE_CODEVIEW = 2
_MAX_MERGE_GROWTH = 4096


class UnsafeMergeError(Exception):
    """Rung 1 cannot be proven safe for this image; fall back to rung 2."""


def _align_up(value, alignment):
    if alignment <= 0:
        raise RuntimeError("PE FileAlignment must be positive")
    return (value + alignment - 1) // alignment * alignment


def _parse_pe(data):
    """Return the header layout merge/rename need, or raise on garbage."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("cannot normalize debug layout of a non-PE artifact")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("cannot normalize debug layout without a PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if not count or optional + optional_size > len(data) or optional_size < 96:
        raise RuntimeError("cannot normalize debug layout of a truncated PE header")
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x20B:
        directory_offset = 112
    elif magic == 0x10B:
        directory_offset = 96
    else:
        raise RuntimeError(f"cannot normalize debug layout of optional-header magic 0x{magic:04x}")
    table = optional + optional_size
    if table + count * _SECTION_HEADER_SIZE > len(data):
        raise RuntimeError("cannot normalize debug layout of a truncated section table")
    sections = []
    for index in range(count):
        header = table + index * _SECTION_HEADER_SIZE
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, header + 8)
        sections.append({
            "name": bytes(data[header:header + 8]),
            "header": header,
            "vsize": vsize,
            "vaddr": vaddr,
            "rawsize": rawsize,
            "rawptr": rawptr,
        })
    return {
        "pe": pe,
        "optional": optional,
        "optional_size": optional_size,
        "directory_offset": directory_offset,
        "file_align": struct.unpack_from("<I", data, optional + 36)[0],
        "section_align": struct.unpack_from("<I", data, optional + 32)[0],
        "size_of_image": struct.unpack_from("<I", data, optional + 56)[0],
        "table": table,
        "count": count,
        "sections": sections,
    }


def _data_directory(data, layout, index):
    """Return (rva, size) for one data-directory slot (zeros when absent)."""
    offset = layout["optional"] + layout["directory_offset"] + index * 8
    slots = struct.unpack_from("<I", data, layout["optional"] + layout["directory_offset"] - 4)[0]
    if index >= slots or offset + 8 > layout["optional"] + layout["optional_size"]:
        return 0, 0
    return struct.unpack_from("<II", data, offset)


def _rsds_record_count(data, layout):
    """Count CodeView RSDS records exactly the way sanitize_pe_codeview_path
    locates them (PointerToRawData based), so the two agree on every input."""
    debug_rva, debug_size = _data_directory(data, layout, _DEBUG_DIRECTORY_INDEX)
    if not debug_rva or not debug_size:
        return 0
    sections = layout["sections"]
    base = None
    for section in sections:
        if section["vaddr"] <= debug_rva < section["vaddr"] + max(section["vsize"], section["rawsize"]):
            base = section["rawptr"] + (debug_rva - section["vaddr"])
            break
    if base is None or base + debug_size > len(data):
        return 0
    found = 0
    for entry in range(base, base + debug_size, _DEBUG_ENTRY_SIZE):
        if entry + _DEBUG_ENTRY_SIZE > len(data):
            break
        dtype = struct.unpack_from("<I", data, entry + 12)[0]
        dsize = struct.unpack_from("<I", data, entry + 16)[0]
        dptr = struct.unpack_from("<I", data, entry + 24)[0]
        if dtype != _DEBUG_TYPE_CODEVIEW or dptr + dsize > len(data):
            continue
        if data[dptr:dptr + 4] == b"RSDS" and dsize > 24:
            found += 1
    return found


def _debug_entries(data, layout):
    """All IMAGE_DEBUG_DIRECTORY entries as (offset, type, size, addr, ptr)."""
    debug_rva, debug_size = _data_directory(data, layout, _DEBUG_DIRECTORY_INDEX)
    if not debug_rva or not debug_size:
        return []
    base = None
    for section in layout["sections"]:
        if section["vaddr"] <= debug_rva < section["vaddr"] + max(section["vsize"], section["rawsize"]):
            base = section["rawptr"] + (debug_rva - section["vaddr"])
            break
    if base is None or base + debug_size > len(data):
        raise UnsafeMergeError("debug directory is not backed by file data")
    entries = []
    for entry in range(base, base + debug_size, _DEBUG_ENTRY_SIZE):
        if entry + _DEBUG_ENTRY_SIZE > len(data):
            raise UnsafeMergeError("truncated debug directory")
        dtype, dsize = struct.unpack_from("<II", data, entry + 12)
        addr, ptr = struct.unpack_from("<II", data, entry + 20)
        entries.append((entry, dtype, dsize, addr, ptr))
    return entries


def merge_buildid_into_rdata(data):
    """Rung 1: absorb the .buildid section into .rdata; return (image, report).

    Raises UnsafeMergeError when any invariant cannot be proven for this
    image, in which case the caller falls back to the name rewrite.  Raises
    RuntimeError on structurally broken input.
    """
    layout = _parse_pe(data)
    sections = layout["sections"]
    buildid = [s for s in sections if s["name"] == _BUILDID_NAME]
    rdata = [s for s in sections if s["name"].rstrip(b"\0") == b".rdata"]
    if len(buildid) != 1:
        raise UnsafeMergeError(f"expected exactly one .buildid section, found {len(buildid)}")
    if len(rdata) != 1:
        raise UnsafeMergeError(f"expected exactly one .rdata section, found {len(rdata)}")
    buildid = buildid[0]
    rdata = rdata[0]
    if buildid is sections[-1]:
        raise UnsafeMergeError(".buildid is the last section (SizeOfImage would change)")
    # Table order must also be RVA order and raw order so the header removal
    # and the raw repack keep their meaning.
    for earlier, later in zip(sections, sections[1:]):
        if later["vaddr"] <= earlier["vaddr"] or later["rawptr"] < earlier["rawptr"]:
            raise UnsafeMergeError("section table is not in RVA/raw order")
    if rdata["vaddr"] >= buildid["vaddr"] or rdata["rawptr"] >= buildid["rawptr"]:
        raise UnsafeMergeError(".rdata does not precede .buildid")
    for section in sections:
        if section["rawptr"] + section["rawsize"] > len(data):
            raise RuntimeError("PE section raw data is out of bounds")
    overlay = len(data) - max(s["rawptr"] + s["rawsize"] for s in sections)
    if overlay:
        # Repacking would shift overlay bytes that may reference absolute file
        # offsets (the setup payload footer does).  Rung 2 stays byte-neutral.
        raise UnsafeMergeError(f"image carries a {overlay}-byte overlay")
    if buildid["vsize"] > buildid["rawsize"]:
        raise UnsafeMergeError(".buildid virtual size exceeds its raw size")
    if rdata["vsize"] > rdata["rawsize"]:
        # The merge computes .rdata's new virtual size from its raw size; a
        # virtual tail would be silently dropped (or the record repointed past
        # the end of what anything may reference).
        raise UnsafeMergeError(".rdata virtual size exceeds its raw size")
    if any(section["rawsize"] == 0 for section in sections):
        raise UnsafeMergeError("image has a section without raw data")

    # The debug directory must live inside .buildid, hold exactly one CodeView
    # entry, and no other structured RVA may point into the moved range.
    debug_rva, debug_size = _data_directory(data, layout, _DEBUG_DIRECTORY_INDEX)
    moved_end = buildid["vaddr"] + buildid["vsize"]
    entries = []
    if debug_rva or debug_size:
        if debug_rva < buildid["vaddr"] or debug_rva + debug_size > moved_end:
            raise UnsafeMergeError("debug directory does not live inside .buildid")
        entries = _debug_entries(data, layout)
        if len(entries) != 1 or entries[0][1] != _DEBUG_TYPE_CODEVIEW:
            raise UnsafeMergeError(f"expected one CodeView debug entry, found {len(entries)}")
    for index in range(16):
        if index == _DEBUG_DIRECTORY_INDEX:
            continue
        rva, size = _data_directory(data, layout, index)
        if rva and rva < moved_end and rva + max(size, 1) > buildid["vaddr"]:
            raise UnsafeMergeError(f"data directory {index} points into .buildid")
    _assert_no_rva_references(data, layout, buildid)

    record_rva = record_size = record_ptr = entry_offset = None
    if entries:
        entry_offset, _, record_size, record_rva, record_ptr = entries[0]
        if record_rva < buildid["vaddr"] or record_rva + record_size > moved_end:
            raise UnsafeMergeError("CodeView record does not live inside .buildid")
        if record_ptr != buildid["rawptr"] + (record_rva - buildid["vaddr"]):
            raise UnsafeMergeError("CodeView record PointerToRawData is inconsistent")

    # The moved bytes land right after .rdata's raw end: nothing is overwritten
    # or discarded (.rdata's alignment slack is promoted into its virtual size
    # instead), and the RSDS record bytes travel verbatim.
    block = bytes(data[buildid["rawptr"]:buildid["rawptr"] + buildid["rawsize"]])
    rdata_vsize_new = rdata["rawsize"] + buildid["rawsize"]
    next_rva = min((s["vaddr"] for s in sections
                    if s is not buildid and s["vaddr"] > rdata["vaddr"]), default=None)
    if next_rva is not None and rdata["vaddr"] + rdata_vsize_new > next_rva:
        raise UnsafeMergeError(
            f".rdata growth reaches 0x{rdata['vaddr'] + rdata_vsize_new:x}, "
            f"past the next section at 0x{next_rva:x}")
    rdata_rawsize_new = _align_up(rdata_vsize_new, layout["file_align"])
    growth = rdata_rawsize_new - rdata["rawsize"] - buildid["rawsize"]
    if growth > _MAX_MERGE_GROWTH:
        raise RuntimeError(f"merge would grow the image by {growth} bytes (max {_MAX_MERGE_GROWTH})")

    # Repack raw offsets contiguously on FileAlignment boundaries, dropping
    # .buildid; every other section keeps its raw bytes verbatim.
    remaining = [s for s in sections if s is not buildid]
    anchor = min(s["rawptr"] for s in remaining)
    if anchor % layout["file_align"]:
        raise RuntimeError("first section raw offset is not FileAlignment-aligned")
    if anchor < layout["table"] + layout["count"] * _SECTION_HEADER_SIZE:
        raise RuntimeError("section raw data overlaps the header table")
    cursor = anchor
    for section in remaining:
        section["rawptr_new"] = cursor
        section["rawsize_new"] = rdata_rawsize_new if section is rdata else section["rawsize"]
        cursor = _align_up(cursor + section["rawsize_new"], layout["file_align"])
    image_end = remaining[-1]["rawptr_new"] + remaining[-1]["rawsize_new"]

    out = bytearray(image_end)
    out[0:anchor] = bytes(data[0:anchor])
    for section in remaining:
        start = section["rawptr_new"]
        out[start:start + section["rawsize"]] = data[section["rawptr"]:section["rawptr"] + section["rawsize"]]
    rdata_block_at = rdata["rawptr_new"] + rdata["rawsize"]
    out[rdata_block_at:rdata_block_at + len(block)] = block

    # Patch the headers: count, table compaction, .rdata fields, debug dir.
    struct.pack_into("<H", out, layout["pe"] + 6, layout["count"] - 1)
    table = layout["table"]
    kept = [s for s in sections if s is not buildid]
    for index, section in enumerate(kept):
        header = table + index * _SECTION_HEADER_SIZE
        out[header:header + _SECTION_HEADER_SIZE] = data[section["header"]:section["header"] + _SECTION_HEADER_SIZE]
        struct.pack_into("<I", out, header + 8, rdata_vsize_new if section is rdata else section["vsize"])
        struct.pack_into("<I", out, header + 16, section["rawsize_new"])
        struct.pack_into("<I", out, header + 20, section["rawptr_new"])
    tail = table + len(kept) * _SECTION_HEADER_SIZE
    out[tail:tail + _SECTION_HEADER_SIZE] = b"\0" * _SECTION_HEADER_SIZE

    report = {"rung": 1, "growth": growth,
              "record_rva": record_rva, "record_size": record_size}
    if entries:
        block_offset_in_rdata = rdata["rawsize"]
        record_shift = block_offset_in_rdata + (record_rva - buildid["vaddr"])
        record_rva_new = rdata["vaddr"] + record_shift
        record_ptr_new = rdata["rawptr_new"] + record_shift
        entry_new = rdata_block_at + (entry_offset - buildid["rawptr"])
        debug_rva_new = rdata["vaddr"] + block_offset_in_rdata + (debug_rva - buildid["vaddr"])
        # The assertions that make the move provable: the relocated structures
        # lie inside .rdata's new virtual range and the record bytes are the
        # very bytes the linker wrote.
        for start, size, what in ((debug_rva_new, debug_size, "debug directory"),
                                  (record_rva_new, record_size, "CodeView record")):
            if start < rdata["vaddr"] or start + size > rdata["vaddr"] + rdata_vsize_new:
                raise RuntimeError(f"merged {what} falls outside .rdata's new virtual range")
        if bytes(out[record_ptr_new:record_ptr_new + record_size]) != \
                bytes(data[record_ptr:record_ptr + record_size]):
            raise RuntimeError("merged CodeView record bytes changed")
        struct.pack_into("<I", out, entry_new + 20, record_rva_new)
        struct.pack_into("<I", out, entry_new + 24, record_ptr_new)
        directory = layout["optional"] + layout["directory_offset"] + _DEBUG_DIRECTORY_INDEX * 8
        struct.pack_into("<II", out, directory, debug_rva_new, debug_size)
        report["record_rva_new"] = record_rva_new
    return bytes(out), report


def _assert_no_rva_references(data, layout, buildid):
    """Refuse the merge when .pdata or .reloc reference RVAs inside .buildid:
    the merge moves content to a new RVA, so any surviving pointer into the
    old range would break.  Directory *contents* that are themselves RVA
    tables get scanned; anything else is covered by the directory scan."""
    moved_end = buildid["vaddr"] + buildid["vsize"]

    def inside(rva):
        return buildid["vaddr"] <= rva < moved_end

    exception_rva, exception_size = _data_directory(data, layout, _EXCEPTION_DIRECTORY_INDEX)
    if exception_rva and exception_size:
        offset = _rva_to_raw(layout, exception_rva)
        if offset is None or offset + exception_size > len(data):
            raise UnsafeMergeError("exception directory is not backed by file data")
        # Zig's ARM64 images carry trailing partial RUNTIME_FUNCTION strides
        # (real sizes: 10224, 10072, 8984), so the loader's stride walk would
        # leave words unchecked.  Scanning every aligned word as a potential
        # RVA covers the whole table whatever the true entry size.
        for word in range(0, exception_size, 4):
            if offset + word + 4 > len(data):
                raise UnsafeMergeError("exception directory is not backed by file data")
            value = struct.unpack_from("<I", data, offset + word)[0]
            if inside(value):
                raise UnsafeMergeError("exception directory references .buildid")
    reloc_rva, reloc_size = _data_directory(data, layout, _RELOC_DIRECTORY_INDEX)
    if reloc_rva and reloc_size:
        offset = _rva_to_raw(layout, reloc_rva)
        if offset is None or offset + reloc_size > len(data):
            raise UnsafeMergeError("reloc directory is not backed by file data")
        cursor = offset
        limit = offset + reloc_size
        while cursor + 8 <= limit:
            page, block_size = struct.unpack_from("<II", data, cursor)
            if block_size < 8 or cursor + block_size > limit:
                raise UnsafeMergeError("malformed base-relocation block")
            for index in range((block_size - 8) // 2):
                entry = struct.unpack_from("<H", data, cursor + 8 + index * 2)[0]
                if entry >> 12 and inside(page + (entry & 0xFFF)):
                    raise UnsafeMergeError("base relocations reference .buildid")
            cursor += block_size


def _rva_to_raw(layout, rva):
    for section in layout["sections"]:
        if section["vaddr"] <= rva < section["vaddr"] + max(section["vsize"], section["rawsize"]):
            return section["rawptr"] + (rva - section["vaddr"])
    return None


def _rename_buildid(data, layout):
    """Rung 2: the 8-byte section-name rewrite; returns the rename count."""
    renamed = 0
    for section in layout["sections"]:
        if section["name"] == _BUILDID_NAME:
            header = section["header"]
            data[header:header + 8] = _RDATA_NAME
            renamed += 1
    return renamed


def normalize_pe_debug_layout(pe_path, pdb_path=None, require_rsds=True):
    """Sanitize the RSDS name, then normalize the .buildid layout in-place.

    `pdb_path` names the symbol file whose basename (with a .pdb extension)
    replaces the CodeView path; it defaults to `pe_path`, which is the right
    answer when the PE was linked under the symbol file's own stem.  Returns
    {"rung", "renamed", "growth"} and fails loudly on anything that is not a
    structurally readable PE.  `require_rsds` demands a CodeView record (the
    GUI/service contract); installer stubs linked without /debug have none.
    """
    with open(pe_path, "rb") as handle:
        data = handle.read()
    layout = _parse_pe(data)
    records = _rsds_record_count(data, layout)
    if records == 1:
        name = os.path.splitext(os.path.basename(pdb_path or pe_path))[0] + ".pdb"
        sanitize_pe_codeview_path(pe_path, name)
        print(f"  RSDS PDB record -> {name}")
        with open(pe_path, "rb") as handle:
            data = handle.read()
    elif records == 0 and require_rsds:
        raise RuntimeError(f"{pe_path}: expected one RSDS CodeView record, found 0")
    elif records > 1:
        raise RuntimeError(f"{pe_path}: expected one RSDS CodeView record, found {records}")

    layout = _parse_pe(data)
    if not any(s["name"] == _BUILDID_NAME for s in layout["sections"]):
        print("  PE debug layout: clean (no .buildid section)")
        return {"rung": 0, "renamed": 0, "growth": 0}

    try:
        result, report = merge_buildid_into_rdata(data)
    except UnsafeMergeError as reason:
        print(f"  PE debug layout: rung 2 name rewrite (.buildid -> .rdata: {reason})")
        result = bytearray(data)
        renamed = _rename_buildid(result, layout)
        if renamed > 1:
            raise RuntimeError(f"{pe_path}: PE carries {renamed} .buildid sections, expected at most 1")
        report = {"rung": 2, "renamed": renamed, "growth": 0}
    else:
        print(f"  PE debug layout: .buildid merged into .rdata (file growth {report['growth']} bytes)")
    result = bytes(result)
    post = _parse_pe(result)
    if any(s["name"] == _BUILDID_NAME for s in post["sections"]):
        raise RuntimeError(f"{pe_path}: .buildid section survived normalization")
    with open(pe_path, "wb") as handle:
        handle.write(result)
    return report


# ---------------------------------------------------------------------------
# Self-tests
# ---------------------------------------------------------------------------

def _build_pe(section_specs, dirs=None, file_align=0x200, first_raw=0x400):
    """Deterministic multi-section PE32+ fixture.

    section_specs: [(name8, rva, vsize, rawsize, content)] in table order,
    which is also RVA and raw order.  dirs: {index: (rva, size)}."""
    dirs = dirs or {}
    count = len(section_specs)
    data = bytearray(first_raw + sum(spec[3] for spec in section_specs))
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, count)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<I", data, 0x98 + 32, 0x1000)     # SectionAlignment
    struct.pack_into("<I", data, 0x98 + 36, file_align)
    struct.pack_into("<I", data, 0x98 + 60, first_raw)  # SizeOfHeaders
    struct.pack_into("<I", data, 0x98 + 108, 16)        # NumberOfRvaAndSizes
    for index, (rva, size) in dirs.items():
        struct.pack_into("<II", data, 0x98 + 112 + index * 8, rva, size)
    table = 0x98 + 0xF0
    offset = first_raw
    image_end = 0
    for index, (name, rva, vsize, rawsize, content) in enumerate(section_specs):
        header = table + index * 40
        data[header:header + 8] = name
        struct.pack_into("<IIII", data, header + 8, vsize, rva, rawsize, offset)
        struct.pack_into("<I", data, header + 36, 0x40000040)
        data[offset:offset + len(content)] = content
        offset += rawsize
        image_end = max(image_end, rva + vsize)
    struct.pack_into("<I", data, 0x98 + 56, _align_up(image_end, 0x1000))
    return bytes(data)


def _buildid_content(record_rva, record_ptr, path=b"fix.pdb\0", extra_entry=False):
    """The debug entry + RSDS record .buildid carries in LLD-MinGW output."""
    record = b"RSDS" + b"\x11" * 16 + struct.pack("<I", 1) + path
    entry = struct.pack("<IIHHIIII", 0, 0, 0, 0, _DEBUG_TYPE_CODEVIEW,
                        len(record), record_rva, record_ptr)
    content = entry + record
    if extra_entry:
        content += struct.pack("<IIHHIIII", 0, 0, 0, 0, 0, 0, 0, 0)
    return content, record


def run_self_tests():
    """Deterministic checks for both rungs and their loud failure modes."""
    failures = []

    def expect(condition, label):
        if not condition:
            failures.append(label)

    def expect_raises(callable_, label, error=RuntimeError):
        try:
            callable_()
        except error:
            return
        except Exception as other:  # noqa: BLE001 - wrong failure counts as failure
            failures.append(f"{label} failed with {type(other).__name__}: {other}")
            return
        failures.append(f"{label} was accepted")

    text = bytes((i * 7 + 3) & 0xFF for i in range(0x100))
    rdata_body = bytes((i * 11 + 5) & 0xFF for i in range(0x80))
    pdata_ok = struct.pack("<III", 0x1000, 0x1100, 0x1000)
    reloc_ok = struct.pack("<IIHH", 0x1000, 12, 0x3010, 0)

    def happy_fixture(record_ptr=None, **overrides):
        content, record = _buildid_content(
            0x301C, 0x81C if record_ptr is None else record_ptr,
            path=b"placeholder.pdb\0")
        specs = [
            (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
            (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
            (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
            (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
            (b".pdata\0\0", 0x5000, 12, 0x200, pdata_ok),
            (b".reloc\0\0", 0x6000, 12, 0x200, reloc_ok),
        ]
        dirs = {3: (0x5000, 12), 5: (0x6000, 12), 6: (0x3000, 28)}
        for key, value in overrides.items():
            if key == "specs":
                specs = value
            elif key == "dirs":
                dirs = value
        return _build_pe(specs, dirs), record

    # --- Rung 1: the happy path keeps every invariant. ---
    fixture, record = happy_fixture()
    try:
        merged, report = merge_buildid_into_rdata(fixture)
    except Exception as error:  # noqa: BLE001
        failures.append(f"rung 1 rejected the clean fixture: {error}")
        merged, report = fixture, {"rung": 0, "growth": 0}
    expect(report.get("rung") == 1 and report.get("growth", 0) == 0,
           "rung 1 merges the clean fixture with zero file growth")
    expect(report.get("growth", 0) <= _MAX_MERGE_GROWTH, "merge growth stays within the budget")
    import pe_verify
    expect(pe_verify.pe_section_names(merged) == [".text", ".rdata", ".data", ".pdata", ".reloc"],
           "the merged image drops .buildid and keeps every other section")
    before = _parse_pe(fixture)
    after = _parse_pe(merged)
    expect(after["count"] == before["count"] - 1, "the merged image has one section header less")
    old_by_name = {s["name"]: s for s in before["sections"]}
    for section in after["sections"]:
        old = old_by_name.get(section["name"])
        if section["name"].rstrip(b"\0") == b".rdata":
            expect(old is not None and section["vaddr"] == old["vaddr"],
                   ".rdata keeps its RVA while its virtual size grows")
            continue
        expect(old is not None and section["vaddr"] == old["vaddr"] and section["vsize"] == old["vsize"],
               f"{section['name']!r} keeps its RVA and virtual size")
    expect(struct.unpack_from("<I", merged, after["optional"] + 56)[0] == before["size_of_image"],
           "SizeOfImage is unchanged")
    rdata_new = [s for s in after["sections"] if s["name"].rstrip(b"\0") == b".rdata"][0]
    expect(rdata_new["vsize"] == 0x200 + 0x200 and rdata_new["rawsize"] == 0x400,
           ".rdata absorbs the .buildid raw bytes")
    expect(rdata_new["vaddr"] + rdata_new["vsize"] <= 0x4000,
           "the merged range stays below the next section's RVA")
    debug_rva, debug_size = _data_directory(merged, after, _DEBUG_DIRECTORY_INDEX)
    expect((debug_rva, debug_size) == (0x2200, 28), "the debug directory moves into .rdata")
    entry_off = _rva_to_raw(after, debug_rva)
    addr_raw, ptr_raw = struct.unpack_from("<II", merged, entry_off + 20)
    expect(addr_raw == 0x221C, "the CodeView entry's AddressOfRawData is the record's new RVA")
    expect(rdata_new["vaddr"] <= addr_raw and addr_raw + len(record) <= rdata_new["vaddr"] + rdata_new["vsize"],
           "the record's new RVA lies inside .rdata's new virtual range")
    expect(bytes(merged[ptr_raw:ptr_raw + len(record)]) == record,
           "the RSDS record bytes stay byte-identical")
    for index, section in enumerate(after["sections"]):
        old = old_by_name.get(section["name"])
        if old is None:
            continue
        new_bytes = merged[section["rawptr"]:section["rawptr"] + old["rawsize"]]
        old_bytes = fixture[old["rawptr"]:old["rawptr"] + old["rawsize"]]
        expect(new_bytes == old_bytes, f"{section['name']!r} raw bytes are verbatim")
        expect(section["rawptr"] % after["file_align"] == 0, f"{section['name']!r} raw offset is aligned")
    expect(merged[before["table"] + before["count"] * 40 - 40:before["table"] + before["count"] * 40]
           == b"\0" * 40, "the vacated section header is zeroed")

    # --- Rung 1: every unsafe condition falls back loudly via UnsafeMerge. ---
    overlay = fixture + b"OVERLAY"
    expect_raises(lambda: merge_buildid_into_rdata(overlay), "an image with an overlay",
                  UnsafeMergeError)
    content, _ = _buildid_content(0x301C, 0x81C)
    tight, _ = happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x100, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x3F00, 0x4000, rdata_body),
        (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
        (b".data\0\0\0", 0x6000, 0x100, 0x200, b"\xa5" * 0x100),
    ], dirs={6: (0x3000, 28)})
    expect_raises(lambda: merge_buildid_into_rdata(tight), ".rdata growth past the next section",
                  UnsafeMergeError)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(dirs={2: (0x3000, 8), 3: (0x5000, 12),
                                                                      5: (0x6000, 12), 6: (0x3000, 28)})[0]),
                  "another data directory pointing into .buildid", UnsafeMergeError)
    bad_reloc = struct.pack("<IIHH", 0x3000, 12, 0x3010, 0)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
        (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
        (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
        (b".reloc\0\0", 0x6000, 12, 0x200, bad_reloc),
    ])[0]), "a base relocation targeting .buildid", UnsafeMergeError)
    bad_pdata = struct.pack("<III", 0x3000, 0x3010, 0x3000)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
        (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
        (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
        (b".pdata\0\0", 0x5000, 12, 0x200, bad_pdata),
    ])[0]), "an exception entry referencing .buildid", UnsafeMergeError)
    tail_dirs = {3: (0x5000, 16), 5: (0x6000, 12), 6: (0x3000, 28)}

    def pdata_tail(tail):
        return happy_fixture(specs=[
            (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
            (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
            (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
            (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
            (b".pdata\0\0", 0x5000, 16, 0x200, struct.pack("<III", 0x1000, 0x1100, 0x1000) + tail),
            (b".reloc\0\0", 0x6000, 12, 0x200, reloc_ok),
        ], dirs=tail_dirs)[0]

    expect_raises(lambda: merge_buildid_into_rdata(pdata_tail(struct.pack("<I", 0x3000))),
                  "a partial exception stride referencing .buildid", UnsafeMergeError)
    try:
        merged_tail, tail_report = merge_buildid_into_rdata(pdata_tail(struct.pack("<I", 0x12345678)))
        expect(tail_report.get("rung") == 1, "a benign partial exception stride still merges")
    except Exception as error:  # noqa: BLE001
        failures.append(f"rung 1 rejected a benign partial exception stride: {error}")
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
        (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
    ], dirs={6: (0x3000, 28)})[0]), "an image without .rdata", UnsafeMergeError)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(record_ptr=0x820)[0]),
                  "an inconsistent CodeView PointerToRawData", UnsafeMergeError)
    extra, _ = _buildid_content(0x301C, 0x81C, extra_entry=True)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
        (_BUILDID_NAME, 0x3000, len(extra), 0x200, extra),
        (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
    ], dirs={6: (0x3000, 56)})[0]), "two debug directory entries", UnsafeMergeError)
    expect_raises(lambda: merge_buildid_into_rdata(happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
        (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
    ], dirs={6: (0x3000, 28)})[0]), ".buildid as the last section", UnsafeMergeError)

    # --- Rung 1: raw growth stays FileAlignment-padded and in budget. ---
    grow_content, _ = _buildid_content(0x301C, 0x91C)
    grow_fixture, _ = happy_fixture(specs=[
        (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
        (b".rdata\0\0", 0x2000, 0x150, 0x300, rdata_body),
        (_BUILDID_NAME, 0x3000, len(grow_content), 0x200, grow_content),
        (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
    ], dirs={6: (0x3000, 28)})
    try:
        grown, grow_report = merge_buildid_into_rdata(grow_fixture)
        expect(grow_report["growth"] == 0x100, "raw repack growth is the FileAlignment padding")
        expect(grow_report["growth"] <= _MAX_MERGE_GROWTH, "the growth budget holds")
        expect(len(grown) == len(grow_fixture) + 0x100, "the file grows by exactly the padding")
    except Exception as error:  # noqa: BLE001
        failures.append(f"rung 1 rejected the raw-growth fixture: {error}")
    huge_content, _ = _buildid_content(0x2001C, 0x681C)
    huge_fa = _build_pe([
        (b".text\0\0\0", 0x1000, 0x100, 0x2000, text),
        (b".rdata\0\0", 0x10000, 0x2800, 0x2800, rdata_body),
        (_BUILDID_NAME, 0x20000, len(huge_content), 0x2000, huge_content),
        (b".data\0\0\0", 0x30000, 0x100, 0x2000, b"\xa5" * 0x100),
    ], dirs={6: (0x20000, 28)}, file_align=0x2000, first_raw=0x2000)
    expect_raises(lambda: merge_buildid_into_rdata(huge_fa), "merge growth beyond the budget")

    # --- normalize_pe_debug_layout: end-to-end file behavior. ---
    import tempfile
    with tempfile.TemporaryDirectory() as work:
        def normalize_fixture(payload, name="fixture.exe", **kwargs):
            path = os.path.join(work, name)
            with open(path, "wb") as handle:
                handle.write(payload)
            report = normalize_pe_debug_layout(path, **kwargs)
            with open(path, "rb") as handle:
                return report, handle.read()

        report, result = normalize_fixture(fixture, pdb_path="fix.pdb")
        expect(report["rung"] == 1, "normalize prefers rung 1 when it is safe")
        sanitized = (b"RSDS" + b"\x11" * 16 + struct.pack("<I", 1)
                     + b"fix.pdb\0" + b"\0" * 7)
        layout = _parse_pe(result)
        record_off = _rva_to_raw(layout, report.get("record_rva_new", 0))
        expect(record_off is not None and bytes(result[record_off:record_off + len(sanitized)]) == sanitized,
               "normalize sanitizes the RSDS name to the .pdb convention")
        report, result = normalize_fixture(overlay)
        expect(report["rung"] == 2 and len(result) == len(overlay),
               "an overlaid image falls back to the byte-neutral rung 2")
        expect(pe_verify.pe_section_names(result) == [".text", ".rdata", ".rdata",
                                                      ".data", ".pdata", ".reloc"],
               "rung 2 removes the .buildid name (duplicate .rdata name is the documented tradeoff)")
        name_at = before["table"] + 2 * _SECTION_HEADER_SIZE
        record_off = before["sections"][2]["rawptr"] + (0x301C - before["sections"][2]["vaddr"])
        allowed = set(range(name_at, name_at + 8))
        allowed |= set(range(record_off + 24, record_off + len(record)))
        diffs = {i for i in range(len(overlay)) if overlay[i] != result[i]}
        expect(result[name_at:name_at + 8] == _RDATA_NAME and diffs <= allowed,
               "rung 2 changes only the name field and the RSDS path field")
        clean_content, _ = _buildid_content(0x201C, 0x61C, path=b"placeholder.pdb\0")
        clean_fixture = _build_pe([
            (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
            (b".rdata\0\0", 0x2000, len(clean_content), 0x200, clean_content),
        ], dirs={6: (0x2000, 28)})
        report, result = normalize_fixture(clean_fixture,
                                           pdb_path=os.path.join(work, "other.debug"))
        expect(report["rung"] == 0, "an image without .buildid is left in place")
        layout = _parse_pe(result)
        record_off = _rva_to_raw(layout, 0x201C)
        expect(bytes(result[record_off + 24:record_off + 34]) == b"other.pdb\0",
               "the .pdb name convention rewrites the CodeView path")
        no_rsds_fixture = _build_pe([
            (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
            (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
            (_BUILDID_NAME, 0x3000, 0x20, 0x200, b"\xa5" * 0x20),
            (b".data\0\0\0", 0x4000, 0x100, 0x200, b"\xa5" * 0x100),
        ], dirs={})
        expect_raises(lambda: normalize_fixture(no_rsds_fixture),
                      "an image missing its RSDS record under require_rsds")
        report, result = normalize_fixture(no_rsds_fixture, require_rsds=False)
        expect(report["rung"] == 1, "an RSDS-less image still merges without the record")
        doubled, _ = happy_fixture(specs=[
            (b".text\0\0\0", 0x1000, 0x190, 0x200, text),
            (b".rdata\0\0", 0x2000, 0x150, 0x200, rdata_body),
            (_BUILDID_NAME, 0x3000, len(content), 0x200, content),
            (_BUILDID_NAME, 0x4000, 0x20, 0x200, b"\xa5" * 0x20),
        ], dirs={6: (0x3000, 28)})
        expect_raises(lambda: normalize_fixture(doubled), "two .buildid sections")
        for label, payload in (("truncated", fixture[:0x100]), ("garbage", b"\0" * 0x400)):
            expect_raises(lambda payload=payload: normalize_fixture(payload), f"a {label} file")

    if failures:
        raise RuntimeError("pe_layout self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_layout self-tests passed")
