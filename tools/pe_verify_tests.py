# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Self-tests and synthetic-PE fixtures for tools/pe_verify.py.

Split out of pe_verify.py so the gate module stays within its line budget;
pe_verify.run_self_tests() delegates here, so the existing invocation runs
these checks unchanged.  The fixtures build deterministic synthetic PE images
in memory and verify nothing is written to disk.  One-way tools/ dependency:
never imports build.py.
"""

import struct

import pe_resources
import pe_strings
from pe_verify import (
    GUI_FORBIDDEN_SERVICE_FUNCTIONS, SERVICE_FORBIDDEN_UI_FUNCTIONS,
    _CHECKSUM_OPTIONAL_OFFSET, _RT_GROUP_ICON, _RT_MANIFEST,
    _checksum_field_offset, _version_translation, pe_image_checksum,
    pe_imports, pe_resource_ids, pe_section_names, version_string_value,
    verify_no_buildid_section, verify_pe_checksum, verify_pe_import_surface,
    verify_service_resources, verify_setup_has_no_decompressor,
    verify_version_identity, verify_windows_binary_imports,
    verify_windows_binary_metadata, verify_windows_manifest_identity,
)


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


def _version_var_entry(key, value):
    """One VarFileInfo Var: header, key, NUL, DWORD padding, binary value."""
    body = key.encode("utf-16le") + b"\0\0"
    header_and_key = 6 + len(body)
    body += b"\0" * ((4 - header_and_key % 4) % 4)
    body += value
    return struct.pack("<HHH", 6 + len(body), len(value), 0) + body


def _manifest_xml(assembly, description, level, comctl32):
    """A minimal but realistic RT_MANIFEST; level=None omits the element."""
    dependency = b""
    if comctl32:
        dependency = (b'<dependency><dependentAssembly><assemblyIdentity type="win32" '
                      b'name="Microsoft.Windows.Common-Controls" version="6.0.0.0" '
                      b'processorArchitecture="*" publicKeyToken="6595b64144ccf1df" '
                      b'language="*"/></dependentAssembly></dependency>')
    elevation = b""
    if level is not None:
        elevation = (b'<trustInfo xmlns="urn:schemas-microsoft-com:asm.v2"><security>'
                     b'<requestedPrivileges><requestedExecutionLevel level="' +
                     level.encode("ascii") +
                     b'" uiAccess="false"/></requestedPrivileges></security></trustInfo>')
    return (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">'
            b'<assemblyIdentity type="win32" name="' + assembly.encode("ascii") +
            b'" processorArchitecture="*"/><description>' + description.encode("ascii") +
            b'</description>' + dependency + elevation + b'</assembly>')


def _write_manifest_resource(data, raw, rva, manifest):
    """Lay out type -> id 1 -> lang 0x0409 -> data entry for one RT_MANIFEST."""
    struct.pack_into("<HH", data, raw + 12, 0, 1)                 # root: one type
    struct.pack_into("<II", data, raw + 16, _RT_MANIFEST, 0x80000018)
    struct.pack_into("<HH", data, raw + 0x18 + 12, 0, 1)          # name dir: id 1
    struct.pack_into("<II", data, raw + 0x28, 1, 0x80000030)
    struct.pack_into("<HH", data, raw + 0x30 + 12, 0, 1)          # lang dir: one
    struct.pack_into("<II", data, raw + 0x40, 0x0409, 0x48)
    struct.pack_into("<II", data, raw + 0x48, rva + 0x58, len(manifest))
    data[raw + 0x58:raw + 0x58 + len(manifest)] = manifest


def _synthetic_manifest_pe(assembly, description, level, comctl32=False):
    """A PE carrying one embedded RT_MANIFEST and nothing else the manifest
    gate looks at."""
    manifest = _manifest_xml(assembly, description, level, comctl32)
    if len(manifest) > 0x3A0:
        raise RuntimeError("synthetic manifest exceeds the fixture buffer")
    data = bytearray(0x600)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    section = 0x98 + 0xF0
    data[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
    struct.pack_into("<I", data, section + 36, 0x40000040)
    struct.pack_into("<II", data, 0x98 + 112 + 2 * 8, 0x1000, 0x400)
    _write_manifest_resource(data, 0x200, 0x1000, manifest)
    return bytes(data)


def _synthetic_imports_pe(dll_functions):
    """One .text section and a real import table for [(dll, [function, ...])]."""
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
    struct.pack_into("<II", data, 0x98 + 112 + 8, 0x1000, 20 * (len(dll_functions) + 1))
    cursor = 0x200 + 20 * (len(dll_functions) + 1)

    def rva(offset):
        return 0x1000 + (offset - 0x200)

    layout = []
    for dll, functions in dll_functions:
        dll_offset = cursor
        cursor += len(dll) + 1
        cursor += -cursor % 8
        thunk_offset = cursor
        cursor += 8 * (len(functions) + 1)
        hints = []
        for function in functions:
            cursor += -cursor % 2
            hints.append((cursor, function))
            cursor += 2 + len(function) + 1
        layout.append((dll, dll_offset, thunk_offset, hints))
    if cursor > 0x400:
        raise RuntimeError("synthetic import table exceeds the fixture buffer")
    for index, (dll, dll_offset, thunk_offset, hints) in enumerate(layout):
        descriptor = 0x200 + index * 20
        struct.pack_into("<IIIII", data, descriptor,
                         rva(thunk_offset), 0, 0, rva(dll_offset), rva(thunk_offset))
        data[dll_offset:dll_offset + len(dll) + 1] = dll.encode("ascii") + b"\0"
        for thunk_index, (hint_offset, function) in enumerate(hints):
            struct.pack_into("<Q", data, thunk_offset + thunk_index * 8, rva(hint_offset))
            struct.pack_into("<H", data, hint_offset, 0)
            data[hint_offset + 2:hint_offset + 3 + len(function)] = \
                function.encode("ascii") + b"\0"
    return bytes(data)


def _synthetic_import_pe(function_name="CreateFileW"):
    return _synthetic_imports_pe([("KERNEL32.dll", [function_name])])


def _synthetic_metadata_pe(original_filename, with_buildid=False,
                           manifest_level="asInvoker", codeview_path="green.pdb"):
    """A synthetic PE that passes every verify_windows_binary_metadata gate
    except, optionally, the .buildid gate: full VERSIONINFO identity with a
    consistent translation block, an embedded RT_MANIFEST, a sanitized RSDS
    record, a GUI-shaped import table, and a correct checksum stamped last."""
    data = bytearray(_synthetic_imports_pe([
        ("user32.dll", ["GetDesktopWindow"]),
        ("gdi32.dll", ["CreateSolidBrush"]),
        ("advapi32.dll", ["RegOpenKeyExW"]),
        ("shell32.dll", ["ShellExecuteW"]),
    ]))
    struct.pack_into("<H", data, 0x80 + 24 + 70, 0x20 | 0x40 | 0x100)
    if with_buildid:
        data[0x188:0x190] = b".buildid"
    data.extend(b"\x00" * (0x800 - len(data)))
    section = 0x98 + 0xF0 + 40                     # second section: .rdata
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x400, 0x2000, 0x400, 0x400)
    struct.pack_into("<I", data, section + 36, 0x40000040)
    struct.pack_into("<H", data, 0x86, 2)
    struct.pack_into("<II", data, 0x98 + 112 + 2 * 8, 0x2000, 0x400)   # .rsrc
    struct.pack_into("<II", data, 0x98 + 112 + 6 * 8, 0x2300, 28)      # debug
    manifest = _manifest_xml("GreenCurve", "Green Curve", manifest_level, False)
    if len(manifest) > 0x280:
        raise RuntimeError("synthetic manifest exceeds the fixture buffer")
    _write_manifest_resource(data, 0x400, 0x2000, manifest)
    struct.pack_into("<II", data, 0x700 + 12, 2, 24 + len(codeview_path) + 1)
    struct.pack_into("<II", data, 0x700 + 20, 0x2320, 0x720)           # addr, raw ptr
    data[0x720:0x724] = b"RSDS"
    data[0x738:0x738 + len(codeview_path) + 1] = codeview_path.encode("utf-8") + b"\0"
    data += b"".join([
        _version_string_entry("CompanyName", "aufkrawall"),
        _version_string_entry("Comments", "Green Curve release build"),
        _version_string_entry("FileDescription", "Green Curve"),
        _version_string_entry("FileVersion", "0.27.0.0"),
        _version_string_entry("InternalName", "GreenCurve"),
        _version_string_entry("LegalCopyright", "Copyright (c) 2026 aufkrawall. MIT License."),
        _version_string_entry("OriginalFilename", original_filename),
        _version_string_entry("ProductName", "Green Curve"),
        _version_string_entry("ProductVersion", "0.27.0.0"),
        _version_var_entry("Translation", struct.pack("<HH", 0x0409, 1200)),
    ])
    data += "040904B0".encode("utf-16le") + b"\0\0"
    struct.pack_into("<I", data, _checksum_field_offset(data), pe_image_checksum(data))
    return bytes(data)


def _synthetic_resource_pe(group_ids):
    """One .rsrc section whose RT_GROUP_ICON directory lists group_ids."""
    data = bytearray(0x400)
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
    struct.pack_into("<HH", data, 0x200 + 12, 0, 2)             # root: two types
    struct.pack_into("<II", data, 0x210, 3, 0x80000100)        # RT_ICON (ignored)
    struct.pack_into("<II", data, 0x218, _RT_GROUP_ICON, 0x80000020)
    struct.pack_into("<HH", data, 0x220 + 12, 0, len(group_ids))
    for index, group in enumerate(group_ids):
        struct.pack_into("<II", data, 0x230 + index * 8, group, 0x80000100)
    return bytes(data)


def run_self_tests():
    """Deterministic checks for the checksum and VERSIONINFO helpers."""
    pe_strings.run_self_tests()
    pe_resources.run_self_tests()
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
    # A ban matches its exact name or the A/W/Ex/ExW spelling Windows exports
    # (SetWindowsHookExW, CreateRemoteThreadEx), never a raw prefix: a ban on
    # CreateProcessW must not catch CreateProcessAsUserW or
    # CreateProcessWithTokenW, the setup's legitimate token relaunch.
    for function, banned, should_match in (
            ("CreateProcessW", "CreateProcessW", True),
            ("CreateProcessW", "CreateProcess", True),
            ("CreateProcessAsUserW", "CreateProcessW", False),
            ("CreateProcessWithTokenW", "CreateProcessW", False),
            ("CreateRemoteThreadEx", "CreateRemoteThread", True),
            ("CreateRemoteThreadEx", "CreateRemoteThreadEx", True),
            ("SetWindowsHookExA", "SetWindowsHookEx", True),
            ("SetWindowsHookExW", "SetWindowsHookEx", True),
            ("SetWindowsHookExW", "setwindowshookex", True),
            ("CreateFileW", "SetWindowsHookEx", False),
            ("CreateFileW", "CreateFile", True)):
        fixture = _synthetic_import_pe(function)
        try:
            verify_pe_import_surface(
                fixture, "import fixture", forbidden_functions={banned})
            expect(not should_match,
                   f"a ban on {banned!r} missed the import {function!r}")
        except RuntimeError:
            expect(should_match,
                   f"a ban on {banned!r} caught the import {function!r}")
    for needle in (b"cabinet.dll", "CreateDecompressor".encode("utf-16le")):
        try:
            verify_setup_has_no_decompressor(import_fixture + needle, "setup fixture")
            failures.append("setup decompressor surface was accepted")
        except RuntimeError:
            pass
    try:
        verify_setup_has_no_decompressor(import_fixture, "setup fixture")
    except RuntimeError as error:
        failures.append(f"setup without decompressor was rejected: {error}")

    expect(pe_section_names(import_fixture) == [".text"], "section names are read back")
    buildid_fixture = bytearray(import_fixture)
    buildid_fixture[0x188:0x190] = b".buildid"
    try:
        verify_no_buildid_section(bytes(buildid_fixture), "buildid fixture")
        failures.append("a .buildid section passed verify_no_buildid_section")
    except RuntimeError:
        pass
    try:
        verify_no_buildid_section(import_fixture, "import fixture")
    except RuntimeError as error:
        failures.append(f"an image without .buildid was rejected: {error}")
    # The gate covers every Windows PE, not only x64: before the scope fix an
    # arm64 image carrying .buildid slipped through verify_windows_binary_metadata.
    for with_buildid in (False, True):
        fixture = _synthetic_metadata_pe("greencurve.exe", with_buildid=with_buildid)
        try:
            verify_windows_binary_metadata(
                fixture, "metadata fixture", "greencurve.exe", "arm64", "llvm-mingw")
            expect(not with_buildid,
                   "an arm64 .buildid section passed verify_windows_binary_metadata")
        except RuntimeError as error:
            expect(with_buildid and ".buildid" in str(error),
                   f"the metadata fixture was rejected for the wrong reason: {error}")
    # The banned-string scan is wired into the metadata gates: an otherwise
    # complete image carrying an injection-family name as text must fail on it.
    dirty = bytearray(_synthetic_metadata_pe("greencurve.exe"))
    dirty += b"\x00VirtualAllocEx\0"
    struct.pack_into("<I", dirty, _checksum_field_offset(dirty), pe_image_checksum(dirty))
    try:
        verify_windows_binary_metadata(
            bytes(dirty), "metadata fixture", "greencurve.exe", "arm64", "llvm-mingw")
        failures.append("a banned string passed verify_windows_binary_metadata")
    except RuntimeError as error:
        expect("VirtualAllocEx" in str(error),
               f"the banned-string rejection did not name the hit: {error}")
    # The anti-debug family is a hard import ban for EVERY variant since the
    # MSVC-ABI shim removed the last CRT-inherent import (2026-09-25); before
    # that, IsDebuggerPresent was exempt for clang-cl images, which is exactly
    # what this gate must never accept again.
    for function in ("IsDebuggerPresent", "CheckRemoteDebuggerPresent",
                     "NtQueryInformationProcess"):
        fixture = _synthetic_imports_pe([
            ("user32.dll", ["GetDesktopWindow"]), ("gdi32.dll", ["CreateSolidBrush"]),
            ("advapi32.dll", ["RegOpenKeyExW"]), ("shell32.dll", ["ShellExecuteW"]),
            ("kernel32.dll", [function]),
        ])
        try:
            verify_windows_binary_imports(fixture, "import fixture", "greencurve.exe")
            failures.append(f"{function} passed the universal import bans")
        except RuntimeError:
            pass
    # Positive control: the same fixture shape with a benign kernel32 import
    # passes, so the rejections above came from the ban list.
    try:
        verify_windows_binary_imports(_synthetic_imports_pe([
            ("user32.dll", ["GetDesktopWindow"]), ("gdi32.dll", ["CreateSolidBrush"]),
            ("advapi32.dll", ["RegOpenKeyExW"]), ("shell32.dll", ["ShellExecuteW"]),
            ("kernel32.dll", ["GetModuleHandleW"]),
        ]), "import fixture", "greencurve.exe")
    except RuntimeError as error:
        failures.append(f"a clean import fixture was rejected: {error}")
    # Elevation and the comctl32 v6 SxS token come from the parsed RT_MANIFEST
    # resource, gated per binary exactly as the generators emit them.
    for assembly, description, level, comctl, filename, accepted in (
            ("GreenCurve", "Green Curve", "asInvoker", False, "greencurve.exe", True),
            ("GreenCurve", "Green Curve", "requireAdministrator", False,
             "greencurve.exe", False),
            ("GreenCurve", "Green Curve", None, False, "greencurve.exe", False),
            ("GreenCurveService", "Green Curve background service", "asInvoker", False,
             "greencurve-service.exe", True),
            ("GreenCurveService", "Green Curve background service", "asInvoker", True,
             "greencurve-service.exe", False),
            ("GreenCurveSetup", "Green Curve setup", "requireAdministrator", True,
             "greencurve-0.27.0-windows-x64-setup.exe", True),
            ("GreenCurveSetup", "Green Curve setup", "asInvoker", True,
             "greencurve-0.27.0-windows-x64-setup.exe", False),
            ("GreenCurveUninstall", "Green Curve uninstaller", "requireAdministrator", True,
             "greencurve-uninstall.exe", True),
            ("GreenCurveUninstall", "Green Curve uninstaller", "requireAdministrator", False,
             "greencurve-uninstall.exe", False)):
        fixture = _synthetic_manifest_pe(assembly, description, level, comctl)
        try:
            verify_windows_manifest_identity(fixture, "manifest fixture", filename)
            expect(accepted, f"the manifest of {filename} was accepted wrongly "
                             f"(level={level}, comctl32={comctl})")
        except RuntimeError:
            expect(not accepted, f"the manifest of {filename} was rejected wrongly "
                                 f"(level={level}, comctl32={comctl})")
    # The elevation gate reads the resource: the whole-file substring checks
    # alone would accept this GUI claiming requireAdministrator.
    bad_level = _synthetic_metadata_pe("greencurve.exe", manifest_level="requireAdministrator")
    try:
        verify_windows_binary_metadata(
            bad_level, "metadata fixture", "greencurve.exe", "arm64", "llvm-mingw")
        failures.append("a GUI manifest demanding requireAdministrator passed the gates")
    except RuntimeError as error:
        expect("requestedExecutionLevel" in str(error),
               f"the elevation rejection did not name the level: {error}")
    # The CodeView record must name its PDB by bare basename.
    bad_pdb = _synthetic_metadata_pe("greencurve.exe", codeview_path="C:\\src\\green.pdb")
    try:
        verify_windows_binary_metadata(
            bad_pdb, "metadata fixture", "greencurve.exe", "arm64", "llvm-mingw")
        failures.append("an absolute CodeView PDB path passed the gates")
    except RuntimeError as error:
        expect("basename" in str(error),
               f"the CodeView rejection did not name the basename rule: {error}")
    for groups, accepted in (([101], True), ([101, 111, 115], False), ([], False)):
        fixture = _synthetic_resource_pe(groups)
        expect(pe_resource_ids(fixture, _RT_GROUP_ICON) == groups,
               f"resource IDs {groups!r} are read back")
        try:
            verify_service_resources(fixture, "resource fixture")
            expect(accepted, f"service icon groups {groups!r} were accepted")
        except RuntimeError:
            expect(not accepted, f"service icon groups {groups!r} were rejected")
    for banned, what in ((SERVICE_FORBIDDEN_UI_FUNCTIONS, "service UI-import"),
                         (GUI_FORBIDDEN_SERVICE_FUNCTIONS, "GUI service-import")):
        try:
            verify_pe_import_surface(
                import_fixture, "import fixture",
                forbidden_functions=banned | {"CreateFileW"})
            failures.append(f"the {what} ban did not apply")
        except RuntimeError:
            pass
        # ...and does not reject an image that imports none of them.
        try:
            verify_pe_import_surface(
                import_fixture, "import fixture", forbidden_functions=banned)
        except RuntimeError as error:
            failures.append(f"the {what} ban rejected a clean image: {error}")
    expect(not (SERVICE_FORBIDDEN_UI_FUNCTIONS & GUI_FORBIDDEN_SERVICE_FUNCTIONS),
           "the two per-binary bans name disjoint APIs")

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
        _version_string_entry("Comments", "Green Curve release build"),
        _version_string_entry("FileDescription", "Green Curve background service"),
        _version_string_entry("FileVersion", "0.27.0.0"),
        _version_string_entry("InternalName", "GreenCurveService"),
        _version_string_entry("LegalCopyright", "Copyright (c) 2026 aufkrawall. MIT License."),
        _version_string_entry("OriginalFilename", "greencurve-service.exe"),
        _version_string_entry("ProductName", "Green Curve"),
        _version_string_entry("ProductVersion", "0.27.0.0"),
        _version_var_entry("Translation", struct.pack("<HH", 0x0409, 1200)),
        "040904B0".encode("utf-16le") + b"\0\0",
    ])
    expect(version_string_value(identity, "OriginalFilename") == "greencurve-service.exe",
           "OriginalFilename is read back")
    expect(version_string_value(identity, "CompanyName") == "aufkrawall", "CompanyName is read back")
    expect(version_string_value(identity, "LegalTrademarks") is None, "an absent key reads as None")
    expect(_version_translation(identity) == (0x0409, 1200), "the Translation pair is read back")
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
    # Every metadata key the generators emit is required: an incomplete
    # VERSIONINFO (like the pre-gate Comments gap) must fail.
    for key in ("Comments", "FileVersion", "LegalCopyright", "ProductVersion"):
        stripped = identity.replace(key.encode("utf-16le"), ("X" * len(key)).encode("utf-16le"))
        try:
            verify_version_identity(stripped, "greencurve-service.exe", "service")
            failures.append(f"a binary without {key} passed verify_version_identity")
        except RuntimeError:
            pass
    # The VarFileInfo Translation must match the StringFileInfo block name.
    wrong_translation = identity.replace(struct.pack("<HH", 0x0409, 1200),
                                         struct.pack("<HH", 0x0407, 1200))
    try:
        verify_version_identity(wrong_translation, "greencurve-service.exe", "service")
        failures.append("a non-US-English Translation passed verify_version_identity")
    except RuntimeError:
        pass
    no_block = identity.replace("040904B0".encode("utf-16le"), "0409XXXX".encode("utf-16le"))
    try:
        verify_version_identity(no_block, "greencurve-service.exe", "service")
        failures.append("an inconsistent StringFileInfo block passed verify_version_identity")
    except RuntimeError:
        pass

    if failures:
        raise RuntimeError("pe_verify self-tests failed:\n  " + "\n  ".join(failures))
    print("pe_verify self-tests passed")
