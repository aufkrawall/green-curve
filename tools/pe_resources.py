# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""PE resource-tree and CodeView parsing behind the artifact metadata gates.

tools/pe_verify.py gates elevation and the comctl32 v6 SxS token from the
PARSED RT_MANIFEST resource (a whole-file substring would also match payload
bytes and dead strings), and the shipped PDB name from the parsed RSDS
CodeView record.  This module owns that structure parsing plus the two
policies that are pure functions of the parsed bytes.

One-way tools/ dependency: imports stdlib only, never build.py.
"""

import struct
import xml.etree.ElementTree as ElementTree

_RT_MANIFEST = 24
_DEBUG_CODEVIEW = 2
_COMCTL32_V6_TOKEN = "6595b64144ccf1df"


# PE layout primitives, shared with tools/pe_verify.py (imported there by
# name so its existing call sites stay unchanged).
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


def _pe_resource_root(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    if pe + 24 + 112 + 3 * 8 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE optional header")
    sections = _sections_of(data, pe, optional)
    rsrc_rva, rsrc_size = _pe_data_directory(data, 2)
    if not rsrc_rva or not rsrc_size:
        return None, sections
    root = _rva_to_offset(sections, rsrc_rva)
    if root is None:
        raise RuntimeError("PE resource directory is not backed by file data")
    return root, sections


def _resource_entries(data, offset):
    if offset + 16 > len(data):
        raise RuntimeError("truncated PE resource directory")
    named, numbered = struct.unpack_from("<HH", data, offset + 12)
    if offset + 16 + (named + numbered) * 8 > len(data):
        raise RuntimeError("truncated PE resource directory entries")
    return [struct.unpack_from("<II", data, offset + 16 + i * 8)
            for i in range(named + numbered)]


def pe_resource_data(data, type_id):
    """[(resource_id, bytes)] of one numbered resource type.

    Walks the three directory levels the PE format defines (type -> name/id
    -> language -> data entry); named entries are skipped."""
    root, sections = _pe_resource_root(data)
    if root is None:
        return []
    resources = []
    for type_name, type_target in _resource_entries(data, root):
        if type_name & 0x80000000 or type_name != type_id:
            continue
        if not type_target & 0x80000000:
            raise RuntimeError("PE resource type entry is not a directory")
        for name, name_target in _resource_entries(data, root + (type_target & 0x7FFFFFFF)):
            if not name_target & 0x80000000:
                raise RuntimeError("PE resource name entry is not a directory")
            for _, lang_target in _resource_entries(
                    data, root + (name_target & 0x7FFFFFFF)):
                if lang_target & 0x80000000:
                    raise RuntimeError("PE resource language entry is not a data entry")
                entry = root + lang_target
                if entry + 16 > len(data):
                    raise RuntimeError("truncated PE resource data entry")
                data_rva, size = struct.unpack_from("<II", data, entry)
                offset = _rva_to_offset(sections, data_rva)
                if offset is None or offset + size > len(data):
                    raise RuntimeError("PE resource data is not backed by file data")
                resources.append((name, bytes(data[offset:offset + size])))
    return resources


def manifest_execution_level(manifest):
    """The requestedExecutionLevel level attribute of an embedded manifest,
    or None when the element is absent.  Fails closed on malformed XML."""
    try:
        root = ElementTree.fromstring(manifest)
    except ElementTree.ParseError as error:
        raise RuntimeError(f"embedded manifest is not well-formed XML: {error}")
    for element in root.iter():
        if element.tag.split("}")[-1] == "requestedExecutionLevel":
            return element.get("level")
    return None


def manifest_has_comctl32_v6(manifest):
    """Whether the manifest opts into the comctl32 v6 common-controls SxS."""
    return _COMCTL32_V6_TOKEN.encode("ascii") in bytes(manifest)


def codeview_pdb_paths(data):
    """PDB paths from the RSDS CodeView records in the debug directory."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    if pe + 24 + 112 + 7 * 8 > len(data) or data[pe:pe + 4] != b"PE\x00\x00":
        raise RuntimeError("invalid PE optional header")
    sections = _sections_of(data, pe, optional)
    debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + 6 * 8)
    if not debug_rva:
        return []
    debug_offset = _rva_to_offset(sections, debug_rva)
    if debug_offset is None:
        raise RuntimeError("PE debug directory is not backed by file data")
    paths = []
    for entry in range(debug_offset, debug_offset + debug_size, 28):
        if entry + 28 > len(data):
            raise RuntimeError("truncated PE debug directory")
        debug_type = struct.unpack_from("<I", data, entry + 12)[0]
        data_size = struct.unpack_from("<I", data, entry + 16)[0]
        data_pointer = struct.unpack_from("<I", data, entry + 24)[0]
        if debug_type != _DEBUG_CODEVIEW:
            continue
        if data_size <= 24 or data_pointer + data_size > len(data):
            raise RuntimeError("PE CodeView record is truncated")
        record = data[data_pointer:data_pointer + data_size]
        if record[:4] != b"RSDS":
            continue
        end = record.find(b"\0", 24)
        if end < 0:
            raise RuntimeError("PE CodeView record has an unterminated path")
        paths.append(record[24:end].decode("ascii", "replace"))
    return paths


def verify_codeview_pdb_basename(data, label):
    """Every RSDS CodeView record must name its PDB by bare basename.

    An absolute path leaks the private build workspace to any downloader, and
    the CodeView sanitizer replaces it with a basename anyway -- so a path
    with a separator here means sanitization did not run.  An image with no
    RSDS record (some installer stubs link without debug info) has no path to
    leak and passes."""
    for path in codeview_pdb_paths(data):
        if not path or any(character in path for character in "\\/:"):
            raise RuntimeError(f"{label}: CodeView PDB path is not a bare basename: {path!r}")


def _synthetic_resource_pe(type_id, payload):
    """One .rsrc section holding a single numbered resource (type -> id 1 ->
    lang 0x0409 -> data entry -> payload)."""
    data = bytearray(0x800)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    section = 0x98 + 0xF0
    data[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<II", data, 0x98 + 112 + 2 * 8, 0x1000, 0x200)
    struct.pack_into("<HH", data, 0x200 + 12, 0, 1)               # root: one type
    struct.pack_into("<II", data, 0x210, type_id, 0x80000018)     # -> name dir
    struct.pack_into("<HH", data, 0x218 + 12, 0, 1)               # name dir: id 1
    struct.pack_into("<II", data, 0x228, 1, 0x80000030)           # -> lang dir
    struct.pack_into("<HH", data, 0x230 + 12, 0, 1)               # lang dir: one
    struct.pack_into("<II", data, 0x240, 0x0409, 0x48)            # -> data entry
    struct.pack_into("<II", data, 0x248, 0x1058, len(payload))    # RVA, size
    data[0x258:0x258 + len(payload)] = payload
    return bytes(data)


def _synthetic_codeview_pe(path, records=1):
    """One .rdata section with RSDS CodeView entries.  `path` is one name for
    all `records` entries, or a list naming each record."""
    paths = [path] * records if isinstance(path, str) else list(path)
    data = bytearray(0x800)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    section = 0x98 + 0xF0
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<II", data, 0x98 + 112 + 6 * 8, 0x1000, 28 * len(paths))
    record_offset = 0x200 + 28 * len(paths)
    for index, name in enumerate(paths):
        entry = 0x200 + index * 28
        path_bytes = name.encode("utf-8") + b"\0"
        size = 24 + len(path_bytes)
        struct.pack_into("<II", data, entry + 12, _DEBUG_CODEVIEW, size)
        struct.pack_into("<II", data, entry + 20, 0, record_offset)   # addr, raw ptr
        data[record_offset:record_offset + 4] = b"RSDS"
        data[record_offset + 24:record_offset + size] = path_bytes
        record_offset += size
    return bytes(data)


def run_self_tests():
    """Deterministic checks for the resource and CodeView parsing."""
    failures = []

    def expect(condition, label):
        if not condition:
            failures.append(label)

    as_invoker = (b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1">'
                  b'<trustInfo><security><requestedPrivileges>'
                  b'<requestedExecutionLevel level="asInvoker" uiAccess="false"/>'
                  b'</requestedPrivileges></security></trustInfo></assembly>')
    require_admin = as_invoker.replace(b"asInvoker", b"requireAdministrator")
    expect(manifest_execution_level(as_invoker) == "asInvoker", "asInvoker is parsed")
    expect(manifest_execution_level(require_admin) == "requireAdministrator",
           "requireAdministrator is parsed")
    namespaced = (b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1">'
                  b'<asmv3:application xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">'
                  b'</asmv3:application></assembly>')
    expect(manifest_execution_level(namespaced) is None,
           "a manifest without requestedExecutionLevel reads as None")
    try:
        manifest_execution_level(b"<assembly>")
        failures.append("malformed manifest XML was accepted")
    except RuntimeError:
        pass
    token = b'publicKeyToken="6595b64144ccf1df"'
    expect(manifest_has_comctl32_v6(token), "the comctl32 v6 token is found")
    expect(not manifest_has_comctl32_v6(as_invoker), "a manifest without the token reads False")

    manifest_pe = _synthetic_resource_pe(_RT_MANIFEST, as_invoker)
    resources = pe_resource_data(manifest_pe, _RT_MANIFEST)
    expect(resources == [(1, as_invoker)], "the RT_MANIFEST resource is read back")
    expect(pe_resource_data(manifest_pe, 16) == [], "an absent resource type reads empty")

    expect(codeview_pdb_paths(_synthetic_codeview_pe("green.pdb")) == ["green.pdb"],
           "a bare-basename CodeView path is read back")
    try:
        verify_codeview_pdb_basename(_synthetic_codeview_pe("green.pdb"), "codeview fixture")
    except RuntimeError as error:
        failures.append(f"a bare-basename CodeView path was rejected: {error}")
    for bad in ("C:\\src\\green.pdb", "src/green.pdb", "C:green.pdb", ""):
        try:
            verify_codeview_pdb_basename(_synthetic_codeview_pe(bad), "codeview fixture")
            failures.append(f"the CodeView path {bad!r} passed the basename gate")
        except RuntimeError:
            pass
    expect(codeview_pdb_paths(_synthetic_codeview_pe(["green.pdb", "other.pdb"]))
           == ["green.pdb", "other.pdb"], "every RSDS record is read back")
    try:
        verify_codeview_pdb_basename(_synthetic_codeview_pe(["green.pdb", "other.pdb"]),
                                     "codeview fixture")
    except RuntimeError as error:
        failures.append(f"clean RSDS records were rejected: {error}")
    try:
        verify_codeview_pdb_basename(_synthetic_codeview_pe(["green.pdb", "src\\other.pdb"]),
                                     "codeview fixture")
        failures.append("an absolute path in a second RSDS record passed the basename gate")
    except RuntimeError:
        pass
    # No RSDS record at all (some installer stubs link without debug info):
    # there is no path to leak.
    no_debug = bytearray(_synthetic_codeview_pe("green.pdb"))
    struct.pack_into("<II", no_debug, 0x98 + 112 + 6 * 8, 0, 0)
    try:
        verify_codeview_pdb_basename(bytes(no_debug), "codeview fixture")
    except RuntimeError as error:
        failures.append(f"an image without a CodeView record was rejected: {error}")

    if failures:
        raise RuntimeError("pe_resources self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_resources self-tests passed")
