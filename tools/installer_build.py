"""Build the Green Curve setup executable and its uninstaller.

Split out of build.py for the same reason security_gates.py and ui_gates.py
were: build.py is under a size ratchet that may only shrink.  The dependency is
one-way — this module never imports build.py; the caller passes its own module
in as `ctx` so paths, flags, and the current version/build number all come from
one place.

What a setup file is
--------------------

    [ installer stub PE ][ compressed GCAR container ][ 44-byte footer ]

The stub is an ordinary hardened Windows executable built from source/installer_*.
The container holds the exact release manifest plus uninstall.exe.  The footer
is read from the end of the file at runtime; the format, and every bounds check
the installer performs on it, live in source/installer_archive_policy.h so
`build.py --test` covers them.

Compression
-----------

XPRESS_HUFF through the Windows Compression API (cabinet.dll).  That is an OS
service rather than a third-party library, so the installer decompresses with
the same API that produced the bytes, and no hand-written entropy decoder ships
to users.  Every payload is decompressed again here and compared before the
setup file is written, so a round-trip failure is a build error, not a support
ticket.  A host without the API (or a payload compression would enlarge) falls
back to storing the container verbatim, which the installer also accepts.
"""

import ctypes
import ctypes.wintypes
import os
import shutil
import struct
import subprocess
import sys

import build_state  # same one-way dependency: it never imports build.py
import msvc_toolchain  # same one-way dependency
import zig_cache  # ditto; owns the cross-process Zig link lock + cache repair

# Mirrors source/installer_archive_policy.h.  The static assertions in that
# header and the struct formats here describe the same bytes; changing one
# without the other is caught by the round-trip verification below.
PAYLOAD_FOOTER_MAGIC = b"GCPAY001"
PAYLOAD_FOOTER_FORMAT = "<8sIQQQII"
PAYLOAD_FOOTER_SIZE = struct.calcsize(PAYLOAD_FOOTER_FORMAT)
ARCHIVE_MAGIC = b"GCAR0001"
ARCHIVE_HEADER_FORMAT = "<8sI"
ARCHIVE_ENTRY_FORMAT = "<64sQQII"
ARCHIVE_ENTRY_SIZE = struct.calcsize(ARCHIVE_ENTRY_FORMAT)
ARCHIVE_MAX_NAME = 63

METHOD_STORE = 0
METHOD_XPRESS_HUFF = 1
COMPRESS_ALGORITHM_XPRESS_HUFF = 4

ARCHIVE_FLAG_NONE = 0
ARCHIVE_FLAG_UNINSTALLER = 1

INSTALLER_SOURCE_NAMES = [
    "installer_main.cpp",
    "installer_ui.cpp",
    "installer_ui_pages.cpp",
    "installer_theme.cpp",
    "installer_apply.cpp",
    "installer_register.cpp",
    "installer_autostart.cpp",
    "installer_payload.cpp",
    "installer_util.cpp",
    "service_acl.cpp",
    "ssp_glue.cpp",
    "cfg_glue.cpp",
    # Toolchain-neutral glue: defines gc_invoke_fatal_dump_hook, which
    # ssp_glue.cpp/cfg_glue.cpp reference. Without it the MinGW link of the
    # setup/uninstaller stubs fails with an undefined symbol (the function
    # used to live in cfg_glue.cpp before the MSVC-ABI split).
    "process_hardening.cpp",
]

# The setup program talks to the SCM, the shell (shortcuts, folder picker), and
# the security APIs; it deliberately links nothing the application links for GPU
# work.
INSTALLER_LINK_LIBS = [
    "-luser32",
    "-lgdi32",
    "-ladvapi32",
    "-lshell32",
    "-lole32",
    "-loleaut32",
    "-luuid",
    "-luxtheme",
    # WTSQueryUserToken / WTSGetActiveConsoleSessionId: setup must reach the
    # interactive session by session id rather than by window, because the
    # in-app updater launches it from the LocalSystem service (session 0) where
    # GetShellWindow() returns nothing.
    "-lwtsapi32",
    # CreateEnvironmentBlock / DestroyEnvironmentBlock.
    "-luserenv",
]
# Deliberately NOT here: -ltaskschd.  The uninstaller talks to Task Scheduler
# through ITaskService, but mingw's libtaskschd.a is a static UUID archive
# rather than an import library, and Zig's arm64 link step refuses it outright
# ("unable to find dynamic system library 'taskschd'").  installer_autostart.cpp
# includes <initguid.h> so taskschd.h emits the two constants it needs, which
# keeps one link line valid for both toolchains.

# requireAdministrator: registering a Windows service and writing under Program
# Files both need it, and asking up front is far better than failing half way
# through an upgrade with the old version already stopped.
INSTALLER_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="GreenCurveSetup" version="VER_STR"
                    processorArchitecture="*"/>
  <description>Green Curve setup</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v2">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <asmv3:application xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
    <asmv3:windowsSettings
      xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">
      <dpiAware>true</dpiAware>
    </asmv3:windowsSettings>
    <asmv3:windowsSettings
      xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>
    </asmv3:windowsSettings>
  </asmv3:application>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
        version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df"
        language="*"/>
    </dependentAssembly>
  </dependency>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
      <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
    </application>
  </compatibility>
</assembly>
"""

# Icon resource id embedded in both setup binaries.  GC_SETUP_ICON_ID in
# source/installer_common.h must name the same number: the resource script
# decides what is *in* the binary, the C++ decides what the window *asks for*,
# and a disagreement means the window silently falls back to the stock Windows
# icon.  check_all() below asserts they agree.
GC_SETUP_ICON_RESOURCE_ID = 101

INSTALLER_RC = """// Generated by tools/installer_build.py. Do not edit by hand.
101 ICON "greencurve.ico"

1 VERSIONINFO
FILEVERSION     VER_MAJOR,VER_MINOR,VER_PATCH,VER_BUILD
PRODUCTVERSION  VER_MAJOR,VER_MINOR,VER_PATCH,VER_BUILD
FILEFLAGSMASK   0x3fL
FILEFLAGS       0x0L
FILEOS          0x40004L
FILETYPE        0x1L
FILESUBTYPE     0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", "DESCRIPTION"
            VALUE "FileVersion", "VER_STR"
            VALUE "InternalName", "GreenCurveSetup"
            VALUE "LegalCopyright", "Copyright (c) 2026 aufkrawall. MIT License."
            VALUE "OriginalFilename", "ORIGINAL_NAME"
            VALUE "ProductName", "Green Curve"
            VALUE "ProductVersion", "VER_STR"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END

1 24 "MANIFEST_NAME"
"""


# ---------------------------------------------------------------------------
# CRC-32 and the container
# ---------------------------------------------------------------------------

def crc32(data):
    """Same polynomial as gc_crc32() in installer_archive_policy.h."""
    import zlib  # stdlib; only the *installer* is barred from third-party code
    return zlib.crc32(data) & 0xFFFFFFFF


def build_archive(entries):
    """Serialize [(name, bytes, flags), ...] into a GCAR container.

    The directory is fixed-size, so every offset is known before any data is
    written and the installer can validate the whole thing before extracting a
    single byte.
    """
    if not entries:
        raise RuntimeError("installer payload would be empty")
    seen = set()
    for name, _, _ in entries:
        encoded = name.encode("utf-8")
        if len(encoded) > ARCHIVE_MAX_NAME:
            raise RuntimeError(f"payload file name too long for the container: {name}")
        if "\\" in name or "/" in name or ":" in name:
            raise RuntimeError(f"payload file name must be a bare name: {name}")
        if name.lower() in seen:
            raise RuntimeError(f"duplicate payload file name: {name}")
        seen.add(name.lower())

    header = struct.pack(ARCHIVE_HEADER_FORMAT, ARCHIVE_MAGIC, len(entries))
    directory_size = ARCHIVE_ENTRY_SIZE * len(entries)
    offset = len(header) + directory_size
    directory = b""
    blob = b""
    for name, data, flags in entries:
        directory += struct.pack(ARCHIVE_ENTRY_FORMAT,
                                 name.encode("utf-8").ljust(64, b"\0"),
                                 offset, len(data), crc32(data), flags)
        blob += data
        offset += len(data)
    return header + directory + blob


# ---------------------------------------------------------------------------
# Compression
# ---------------------------------------------------------------------------

def _compression_api():
    """Resolve cabinet.dll's Compression API, or None when unavailable."""
    if sys.platform != "win32":
        return None
    try:
        cabinet = ctypes.WinDLL("cabinet.dll")
        cabinet.CreateCompressor.argtypes = [ctypes.wintypes.DWORD, ctypes.c_void_p,
                                             ctypes.POINTER(ctypes.c_void_p)]
        cabinet.CreateCompressor.restype = ctypes.wintypes.BOOL
        cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.c_size_t)]
        cabinet.Compress.restype = ctypes.wintypes.BOOL
        cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]
        cabinet.CloseCompressor.restype = ctypes.wintypes.BOOL
        cabinet.CreateDecompressor.argtypes = [ctypes.wintypes.DWORD, ctypes.c_void_p,
                                               ctypes.POINTER(ctypes.c_void_p)]
        cabinet.CreateDecompressor.restype = ctypes.wintypes.BOOL
        cabinet.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_size_t)]
        cabinet.Decompress.restype = ctypes.wintypes.BOOL
        cabinet.CloseDecompressor.argtypes = [ctypes.c_void_p]
        cabinet.CloseDecompressor.restype = ctypes.wintypes.BOOL
        return cabinet
    except (OSError, AttributeError):
        return None


def _xpress_huff_compress(cabinet, data):
    handle = ctypes.c_void_p()
    if not cabinet.CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, None, ctypes.byref(handle)):
        raise RuntimeError(f"CreateCompressor failed (error {ctypes.get_last_error()})")
    try:
        needed = ctypes.c_size_t(0)
        cabinet.Compress(handle, data, len(data), None, 0, ctypes.byref(needed))
        buffer = ctypes.create_string_buffer(max(needed.value, 1))
        produced = ctypes.c_size_t(0)
        if not cabinet.Compress(handle, data, len(data), buffer, len(buffer), ctypes.byref(produced)):
            raise RuntimeError(f"Compress failed (error {ctypes.get_last_error()})")
        return buffer.raw[:produced.value]
    finally:
        cabinet.CloseCompressor(handle)


def _xpress_huff_decompress(cabinet, data, expected_size):
    handle = ctypes.c_void_p()
    if not cabinet.CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, None, ctypes.byref(handle)):
        raise RuntimeError(f"CreateDecompressor failed (error {ctypes.get_last_error()})")
    try:
        buffer = ctypes.create_string_buffer(max(expected_size, 1))
        produced = ctypes.c_size_t(0)
        if not cabinet.Decompress(handle, data, len(data), buffer, len(buffer), ctypes.byref(produced)):
            raise RuntimeError(f"Decompress failed (error {ctypes.get_last_error()})")
        return buffer.raw[:produced.value]
    finally:
        cabinet.CloseDecompressor(handle)


def compress_payload(container):
    """Return (method, blob).  Always verified by decompressing the result."""
    cabinet = _compression_api()
    if cabinet is None:
        print("  installer: Windows Compression API unavailable; storing the payload uncompressed")
        return METHOD_STORE, container
    compressed = _xpress_huff_compress(cabinet, container)
    if len(compressed) >= len(container):
        # Compression made it bigger (tiny or already-compressed payload).
        return METHOD_STORE, container
    round_trip = _xpress_huff_decompress(cabinet, compressed, len(container))
    if round_trip != container:
        raise RuntimeError("payload compression did not round-trip; refusing to write a setup file")
    ratio = 100.0 * len(compressed) / len(container)
    print(f"  installer: payload {len(container):,} -> {len(compressed):,} bytes ({ratio:.1f}%)")
    return METHOD_XPRESS_HUFF, compressed


# ---------------------------------------------------------------------------
# Compiling the stub and the uninstaller
# ---------------------------------------------------------------------------

def _compile_installer_binary_msvc(ctx, output_path, arch, uninstaller, work,
                                   sources, definitions, res_path):
    """Object-first clang-cl build of the setup/uninstaller stubs.

    No private symbols are retained for these stubs (the llvm-mingw path
    strips them via -s), so the build omits -Zi and no PDB is emitted."""
    toolchain = ctx.MSVC_TOOLCHAIN
    glue = {"ssp_glue.cpp", "cfg_glue.cpp"}
    msvc_sources = [path for path in sources
                    if os.path.basename(path) not in glue]
    compile_flags = msvc_toolchain.windows_compile_flags(
        service=False, arch=arch, app_version=ctx.APP_VERSION,
        build_number=ctx.APP_BUILD_NUMBER, source_dir=ctx.SOURCE_DIR,
        debug=False)
    # definitions carries APP_BUILD_NUMBER, which the flag builder already
    # injects; only the uninstaller toggle is added on top.
    compile_flags.extend(flag for flag in definitions
                         if not flag.startswith("-DAPP_BUILD_NUMBER="))
    object_dir = os.path.join(work, "obj-uninstall" if uninstaller else "obj-setup")
    objects = msvc_toolchain.compile_windows_objects(
        toolchain.clang_cl, compile_flags, msvc_sources, object_dir,
        ctx._run_compiler)
    link_flags = msvc_toolchain.windows_link_flags("installer.pdb", arch,
                                                   debug=False)
    if msvc_toolchain.link_windows(
            toolchain.lld_link, link_flags, objects, res_path,
            msvc_toolchain.msvc_link_libs(INSTALLER_LINK_LIBS), output_path,
            ctx._run_compiler) != 0:
        raise RuntimeError(f"installer compilation failed ({arch}, uninstaller={uninstaller})")


def _installer_sources(ctx):
    return [os.path.join(ctx.SOURCE_DIR, name) for name in INSTALLER_SOURCE_NAMES]


def _write_installer_resources(ctx, work, uninstaller):
    """Emit the .rc/.manifest pair and compile them with llvm-rc."""
    major, minor, patch, build = build_state.parse_version_parts(
        ctx.APP_VERSION, ctx.APP_BUILD_NUMBER)
    version_string = f"{major}.{minor}.{patch}.{build}"
    manifest_name = "greencurve-uninstall.manifest" if uninstaller else "greencurve-setup.manifest"
    manifest = INSTALLER_MANIFEST.replace("VER_STR", version_string)
    with open(os.path.join(work, manifest_name), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(manifest)

    rc = (INSTALLER_RC
          .replace("VER_MAJOR", str(major))
          .replace("VER_MINOR", str(minor))
          .replace("VER_PATCH", str(patch))
          .replace("VER_BUILD", str(build))
          .replace("VER_STR", version_string)
          .replace("MANIFEST_NAME", manifest_name)
          .replace("DESCRIPTION", "Green Curve uninstaller" if uninstaller else "Green Curve setup")
          .replace("ORIGINAL_NAME", "uninstall.exe" if uninstaller else "greencurve-setup.exe"))
    rc_name = "greencurve-uninstall.rc" if uninstaller else "greencurve-setup.rc"
    rc_path = os.path.join(work, rc_name)
    with open(rc_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(rc)
    # The icon is referenced by name and resolved relative to the .rc file.
    shutil.copy2(ctx.ICON_ICO, os.path.join(work, "greencurve.ico"))

    res_path = os.path.join(work, rc_name.replace(".rc", ".res"))
    result = subprocess.run([ctx.LLVM_MINGW_RC, "/x", f"/fo{res_path}", rc_path], cwd=work)
    if result.returncode != 0 or not os.path.exists(res_path):
        raise RuntimeError("installer resource compilation failed")
    return res_path


def compile_installer_binary(ctx, output_path, arch, uninstaller, work):
    """Compile one installer-family binary with the project's hardening flags."""
    sources = _installer_sources(ctx)
    definitions = [f"-DAPP_BUILD_NUMBER={ctx.APP_BUILD_NUMBER}"]
    if uninstaller:
        definitions.append("-DGREEN_CURVE_UNINSTALLER=1")
    res_path = _write_installer_resources(ctx, work, uninstaller)

    if ctx.MSVC_TOOLCHAIN is not None:
        _compile_installer_binary_msvc(ctx, output_path, arch, uninstaller,
                                       work, sources, definitions, res_path)
        if not os.path.exists(output_path):
            raise RuntimeError("installer compilation produced no output")
        return output_path
    if arch == "arm64":
        # Same object-first path the application's arm64 build uses, so BTI/PAC
        # survive code generation and the binary passes the same gates.
        object_dir = os.path.join(work, "obj-uninstall" if uninstaller else "obj-setup")
        objects = ctx._compile_arm64_objects(sources, object_dir, "aarch64-windows-gnu", definitions)
        cmd = [ctx.ZIG_EXE, "c++", "-target", "aarch64-windows-gnu",
               "-mbranch-protection=standard", "-fno-lto", "-static",
               "-Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va",
               "-o", output_path, *objects, res_path, *INSTALLER_LINK_LIBS]
    else:
        cmd = [ctx.LLVM_MINGW_CLANG, *ctx.COMMON_FLAGS, *ctx.WINDOWS_FLAGS, *definitions,
               "-o", output_path, *sources, res_path, *INSTALLER_LINK_LIBS]
    if arch == "arm64":
        # Hold the cross-process zig-cache lock: the arm64 installer links via
        # Zig and must not mutate the shared global cache while a matrix
        # build's links are running (see zig_cache for the poisoning failure).
        link_rc = zig_cache.run_zig_link(
            cmd, work, ctx.ZIG_CACHE_ROOTS, log=print,
            audit=lambda text, rc: 1 if ctx._audit_compiler_output(text, True) else rc)
    else:
        link_rc = ctx._run_compiler(cmd, cwd=work, allow_cfg_collision=True)
    if link_rc != 0:
        raise RuntimeError(f"installer compilation failed ({arch}, uninstaller={uninstaller})")
    if arch == "arm64":
        if subprocess.run([ctx.LLVM_MINGW_STRIP, "--strip-all", output_path], cwd=work).returncode != 0:
            raise RuntimeError("installer arm64 strip failed")
    if not os.path.exists(output_path):
        raise RuntimeError("installer compilation produced no output")
    return output_path


# ---------------------------------------------------------------------------
# Assembling the setup file
# ---------------------------------------------------------------------------

def _append_payload(stub_path, output_path, container):
    method, blob = compress_payload(container)
    with open(stub_path, "rb") as handle:
        stub = handle.read()
    if not stub.startswith(b"MZ"):
        raise RuntimeError("installer stub is not a PE image")
    footer = struct.pack(PAYLOAD_FOOTER_FORMAT, PAYLOAD_FOOTER_MAGIC, method,
                         len(stub), len(blob), len(container), crc32(container), 0)
    footer = footer[:-4] + struct.pack("<I", crc32(footer[:-4]))
    with open(output_path, "wb") as handle:
        handle.write(stub)
        handle.write(blob)
        handle.write(footer)
    return method


def _verify_setup_file(path, container):
    """Re-read the finished file exactly the way the installer will."""
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        handle.seek(size - PAYLOAD_FOOTER_SIZE)
        raw_footer = handle.read(PAYLOAD_FOOTER_SIZE)
        magic, method, offset, compressed_size, uncompressed_size, archive_crc, footer_crc = \
            struct.unpack(PAYLOAD_FOOTER_FORMAT, raw_footer)
        if magic != PAYLOAD_FOOTER_MAGIC:
            raise RuntimeError("setup file footer magic is wrong")
        if footer_crc != crc32(raw_footer[:-4]):
            raise RuntimeError("setup file footer checksum is wrong")
        if offset + compressed_size != size - PAYLOAD_FOOTER_SIZE:
            raise RuntimeError("setup file payload does not end at the footer")
        if uncompressed_size != len(container) or archive_crc != crc32(container):
            raise RuntimeError("setup file payload does not match the staged container")
        handle.seek(offset)
        blob = handle.read(compressed_size)
    if method == METHOD_STORE:
        extracted = blob
    else:
        cabinet = _compression_api()
        if cabinet is None:
            raise RuntimeError("a compressed setup file was produced on a host that cannot verify it")
        extracted = _xpress_huff_decompress(cabinet, blob, uncompressed_size)
    if extracted != container:
        raise RuntimeError("setup file payload failed verification")


def build_setup_executable(ctx, arch, payload_dir, expected_names):
    """Build greencurve-<version>-windows-<arch>-setup.exe.

    `payload_dir` is the staged release folder; `expected_names` is the exact
    manifest build.py already validated, so the setup file and the archive can
    never ship different sets of files.
    """
    work = ctx.prepare_work_subdir(f"installer-{arch}")
    try:
        uninstaller_path = os.path.join(work, "uninstall.exe")
        compile_installer_binary(ctx, uninstaller_path, arch, True, work)
        stub_path = os.path.join(work, "setup-stub.exe")
        compile_installer_binary(ctx, stub_path, arch, False, work)
        for binary in (uninstaller_path, stub_path):
            ctx.verify_release_binary(binary, "windows", arch)

        entries = []
        for name in sorted(expected_names):
            source = os.path.join(payload_dir, name)
            if not os.path.isfile(source):
                raise RuntimeError(f"installer payload is missing {name}")
            with open(source, "rb") as handle:
                entries.append((name, handle.read(), ARCHIVE_FLAG_NONE))
        with open(uninstaller_path, "rb") as handle:
            entries.append(("uninstall.exe", handle.read(), ARCHIVE_FLAG_UNINSTALLER))

        container = build_archive(entries)
        output = os.path.join(ctx.SCRIPT_DIR,
                              f"greencurve-{ctx.APP_VERSION}-windows-{arch}-setup.exe")
        if os.path.exists(output):
            os.remove(output)
        _append_payload(stub_path, output, container)
        _verify_setup_file(output, container)
    finally:
        ctx.cleanup_work_subdir(work)

    size = os.path.getsize(output)
    print(f"Built {os.path.basename(output)} ({size:,} bytes / {size / 1024:.1f} KB)")
    with open(output + ".sha256", "w") as handle:
        handle.write(f"{ctx._sha256_file(output)}  {os.path.basename(output)}\n")
    return output


# ---------------------------------------------------------------------------
# Source gates
#
# Invariants a future edit could quietly break, checked by `build.py --test`
# alongside the rest of the source guards.  These are the properties that have
# no unit test because they are about *where* code lives or which API it uses.
# ---------------------------------------------------------------------------

def check_all(ctx, require_text, forbid_text):
    def source(name):
        return os.path.join(ctx.SOURCE_DIR, name)

    palette = source("theme_palette.h")
    app_shared = source("app_shared.h")
    installer_common = source("installer_common.h")
    # One palette, shared by the program and by the setup window.  A duplicated
    # colour table would drift the first time a shade is tweaked, and the two
    # windows appear side by side during an upgrade.
    require_text(palette, "#define COL_BG", "the shared palette defines the window background")
    require_text(app_shared, '#include "theme_palette.h"',
                 "the application takes its palette from the shared header")
    require_text(installer_common, '#include "theme_palette.h"',
                 "the installer takes its palette from the shared header")
    forbid_text(app_shared, "#define COL_BUTTON ",
                "the palette is not redefined next to the shared header")

    # The setup program must not grow a dependency on the application model:
    # app_shared.h drags in the GPU state machine and the service protocol.
    for name in INSTALLER_SOURCE_NAMES:
        if not name.startswith("installer_"):
            continue
        forbid_text(source(name), '#include "app_shared.h"',
                    f"{name} stays independent of the application model")

    # The toolchain-neutral fatal-dump hook definition must link into every
    # Windows binary, including the setup/uninstaller stubs: ssp_glue.cpp and
    # cfg_glue.cpp reference gc_invoke_fatal_dump_hook. Dropping it from this
    # list turns every MinGW stub link into an undefined-symbol failure
    # (2026-08-29 release-packaging CI).
    if "process_hardening.cpp" not in INSTALLER_SOURCE_NAMES:
        raise RuntimeError(
            "INSTALLER_SOURCE_NAMES must link process_hardening.cpp "
            "(toolchain-neutral fatal-dump hook referenced by the glue)")

    payload = source("installer_payload.cpp")
    # Decompression is an OS service, not a vendored library, and not a
    # hand-written entropy decoder whose bugs would corrupt installed binaries.
    require_text(payload, "cabinet.dll", "the installer decompresses with the Windows Compression API")
    require_text(payload, "gc_archive_validate",
                 "the payload is fully validated before anything is extracted")
    require_text(payload, "archiveCrc32",
                 "the extracted payload is checksummed before use")

    apply_shard = source("installer_apply.cpp")
    # Ordering is the correctness property of an upgrade: capture the live
    # settings while the old build is still running, then stop it, then replace
    # its files.  Anchored to gc_install_execute() so the checks read call order
    # inside the one function that owns it, not the order the helpers happen to
    # be defined in further up the file.
    install_anchor = "bool gc_install_execute(GcInstallContext* context)"
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_install_directory_is_secure_rooted(targetDirectory)",
                                   "gc_capture_active_settings(context);",
                                   "the service-root preflight runs before setup disturbs the live installation")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_capture_active_settings(context);",
                                   "gc_stop_gui_processes(context)",
                                   "settings are captured before anything is stopped")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_stop_service(context, &serviceWasRunning)",
                                   "gc_write_payload_file",
                                   "the service is stopped before its binary is replaced")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_register_service(context)",
                                   "gc_reapply_captured_settings(context)",
                                   "settings are re-applied only after the new service is registered")
    require_text(apply_shard, "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH",
                 "payload files are replaced atomically")
    require_text(apply_shard, "--service-install",
                 "the installed binary owns its own SCM registration")
    require_text(apply_shard, "gc_install_paths_equal(registeredUtf8, context->plan.targetDirectory)",
                 "the re-pointed service registration is verified against the new directory")
    require_text(apply_shard, "UNPROTECTED_DACL_SECURITY_INFORMATION",
                 "a moved-from directory is released so the user can delete it")
    require_text(apply_shard, "CreateProcessWithTokenW",
                 "the installed GUI is started unelevated, not with setup's admin token")
    # The stop steps must fail closed when a process handle cannot be taken:
    # "all exited" would otherwise be an assumption while the process still
    # holds the files the next step replaces.
    stop_shard = source("installer_stop.cpp")
    require_text(stop_shard, "F-STOP-GUI-OPENFAIL",
                 "a GUI process that cannot be opened fails the stop step")
    require_text(stop_shard, "F-STOP-SVC-OPENFAIL",
                 "a service process that cannot be opened fails the stop step")
    require_text(stop_shard, "F-STOP-TERM-WAIT",
                 "a terminated GUI process that does not exit still fails the stop step")
    require_text(stop_shard, "F-STOP-GUI-ENUMFAIL",
                 "failed window enumeration is not mistaken for no GUI")
    require_text(stop_shard, "F-STOP-GUI-WAITFAIL",
                 "a multi-process wait API failure fails the stop step")
    require_text(stop_shard, "F-STOP-GUI-PROBEFAIL",
                 "an individual process wait API failure fails the stop step")
    require_text(stop_shard, "wait == WAIT_FAILED",
                 "service and GUI wait failures have explicit diagnostics")
    forbid_text(stop_shard, '#include "app_shared.h"',
                "the stop shard stays independent of the application model")

    # The install runs on a worker thread, and CoInitializeEx is per-thread:
    # without its own call, IShellLink creation failed with CO_E_NOTINITIALIZED
    # and every install silently produced no shortcuts.
    ctx.require_text_in_operation(source("installer_ui.cpp"),
                                  "static DWORD WINAPI gc_worker_thread(LPVOID parameter)",
                                  "CoInitializeEx(",
                                  "the install worker thread initializes COM itself")
    # The settings capture asks the INSTALLED binary first, because only it is
    # guaranteed to speak the protocol the running service speaks -- a payload
    # binary from across a protocol bump reports "no active settings" and the
    # upgrade then restores nothing while looking like a clean run.  It is asked
    # only when the recorded version knows the verb (an older build falls through
    # to opening its GUI and blocks setup), and the payload's binary, which
    # always knows the verb, is the fallback.
    require_text(apply_shard, "gc_payload_find(&context->payload, GC_SETUP_GUI_EXE)",
                 "the settings capture always has the new binary available")
    require_text(apply_shard, "context->plan.captureFromInstalledBinary",
                 "the capture prefers the build that matches the running service")
    ctx.require_order_in_operation(apply_shard,
                                   "static void gc_capture_active_settings(",
                                   'gc_try_export_active_settings(installedExe, snapshot, "installed")',
                                   'gc_try_export_active_settings(exePath, snapshot, "payload")',
                                   "the payload binary is the capture fallback, not the only attempt")
    require_text(source("installer_plan_policy.h"),
                 "gc_version_at_least(prior->version",
                 "an unknown or older installed build is never run with a verb it does not know")
    # The version is a fallback for installs predating the marker; the marker
    # is the authority, because a release number answers a capability question
    # only at release granularity and was already wrong once inside 0.21.
    require_text(source("installer_register.cpp"),
                 "GC_SETUP_SETTINGS_EXPORT_VALUE",
                 "every install records whether its binary knows the settings export verb")
    require_text(apply_shard, "GC_SETUP_SETTINGS_EXPORT_VALUE",
                 "the capture reads the recorded capability back")
    require_text(source("installer_plan_policy.h"),
                 "prior->settingsExport == GC_TOGGLE_ON",
                 "a recorded capability outranks the version comparison")
    require_text(apply_shard, "GC_APP_EXPORT_TIMEOUT_MS",
                 "a binary that turns out not to know the verb is terminated in seconds, not a minute")
    require_text(apply_shard, "gc_create_private_temp_directory",
                 "the elevated capture helper is staged in an unpredictable protected directory")
    require_text(apply_shard, "gc_install_directory_is_secure_rooted",
                 "LocalSystem service installs reject user-writable parent directories")
    installer_ui = source("installer_ui.cpp")
    # The setup window wears the Green Curve icon.  The .rc has always embedded
    # it (Explorer showed it on the file), but the window class asked for
    # IDI_APPLICATION, so the title bar, Alt-Tab and the taskbar all showed the
    # stock Windows executable icon instead.  Both class slots must be filled:
    # hIconSm is the caption icon, hIcon is Alt-Tab/taskbar.
    require_text(installer_common, "#define GC_SETUP_ICON_ID",
                 "the setup binaries name the embedded icon resource")
    require_text(installer_ui, "windowClass.hIcon = gc_load_setup_icon(",
                 "the setup window class carries the Green Curve icon")
    require_text(installer_ui, "windowClass.hIconSm = gc_load_setup_icon(",
                 "the setup caption bar gets a small icon rather than a "
                 "down-scaled large one")
    forbid_text(installer_ui, "windowClass.hIcon = LoadIconW",
                "the setup window class never registers the stock Windows icon")
    require_text(installer_ui, "MAKEINTRESOURCEW(GC_SETUP_ICON_ID)",
                 "the icon comes from the module's own resource")
    # A fast double-click on a BS_OWNERDRAW button is BN_CLICKED followed by
    # BN_DBLCLK (pinned by the native button fixture); filtering to BN_CLICKED
    # alone swallowed the second click of every fast double-click, so rapid
    # page navigation felt like the installer ignored the user.
    require_text(installer_ui, "gc_wizard_notification_is_click(",
                 "wizard notifications route through the pure click policy")
    require_text(installer_ui, "F-CLICK-FILTER",
                 "the fast-double-click handling is marked where it lives")
    forbid_text(installer_ui, "HIWORD(wParam) != BN_CLICKED",
                "the click-only filter that dropped fast double-clicks is gone")
    # The id the window asks for and the id the .rc emits are in different
    # files and different languages; a silent disagreement reproduces exactly
    # the bug this gate exists for.
    if f"\n{GC_SETUP_ICON_RESOURCE_ID} ICON " not in INSTALLER_RC:
        raise RuntimeError(
            f"INSTALLER_RC must emit icon resource {GC_SETUP_ICON_RESOURCE_ID} "
            "to match GC_SETUP_ICON_ID in installer_common.h")
    require_text(installer_common,
                 f"#define GC_SETUP_ICON_ID {GC_SETUP_ICON_RESOURCE_ID}",
                 "the window's icon id matches the one the resource script emits")
    ctx.require_order_in_operation(installer_ui,
                                   "static bool gc_commit_folder_page(GcWizard* wizard)",
                                   "gc_install_directory_is_secure_rooted(chosenWide)",
                                   "StringCchCopyA(wizard->options.directory",
                                   "the folder page rejects unsafe service roots before accepting the choice")
    require_text(apply_shard, "apply_protected_service_dir_dacl",
                 "the install directory is hardened before privileged payload extraction")
    installer_util = source("installer_util.cpp")
    require_text(installer_util, "gc_path_is_direct_child_of_root",
                 "a weak intermediate Program Files directory cannot substitute the service root")
    require_text(installer_util, "FOLDERID_ProgramFiles",
                 "elevated capture helpers stage beneath an administrator-owned parent")
    require_text(installer_util, "CREATE_NEW",
                 "failure logs never truncate or follow a pre-existing attacker-controlled file")
    require_text(installer_util, "FILE_FLAG_OPEN_REPARSE_POINT",
                 "failure logs refuse pre-existing reparse targets")
    forbid_text(installer_util, "CREATE_ALWAYS",
                "the elevated installer cannot clobber a caller-selected file")

    # -----------------------------------------------------------------------
    # Uninstall: what outlives a file deletion
    #
    # The autostart registrations live in Task Scheduler and in per-user Run
    # keys, i.e. nowhere near the install directory or the uninstall key, so
    # they survive everything else the uninstaller does unless it removes them
    # by name.  The names are owned by the application, and the installer cannot
    # include app_shared.h, so the literals are duplicated -- and pinned here.
    # -----------------------------------------------------------------------
    uninstall_policy = source("installer_uninstall_policy.h")
    autostart = source("installer_autostart.cpp")
    require_text(app_shared, '#define STARTUP_TASK_PREFIX "Green Curve Startup - "',
                 "the application owns the logon task name prefix")
    require_text(uninstall_policy, '#define GC_STARTUP_TASK_PREFIX "Green Curve Startup - "',
                 "the uninstaller looks for the same prefix the application registers")
    require_text(source("main_tray_autostart.cpp"),
                 'L"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run"',
                 "the resident tray autostart is an HKCU Run value")
    require_text(uninstall_policy,
                 '#define GC_TRAY_AUTOSTART_RUN_SUBKEY "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run"',
                 "the uninstaller looks in the same Run key the application writes")
    require_text(source("main_tray_autostart.cpp"),
                 'TRAY_AUTOSTART_VALUE_NAME = L"Green Curve"',
                 "the tray autostart value name is a fixed product string")
    require_text(uninstall_policy, '#define GC_TRAY_AUTOSTART_VALUE_NAME "Green Curve"',
                 "the uninstaller removes the same Run value name")
    # Removal happens through the pure predicates, never through an inline
    # rule written against a COM string or a registry buffer: a removal that
    # matches too widely is the one mistake here that cannot be undone.
    require_text(autostart, "gc_uninstall_task_name_is_ours(",
                 "only tasks the prefix rule accepts are deleted")
    require_text(autostart, "gc_uninstall_command_references(commandUtf8, GC_SETUP_GUI_EXE)",
                 "a Run value is removed only when its first executable token is exactly this program")
    require_text(autostart, "RegEnumKeyExW(HKEY_USERS",
                 "every loaded user hive is checked, not just the elevating admin's")
    require_text(autostart, "ITaskService",
                 "logon tasks are enumerated through the documented scheduler API")
    require_text(autostart, "#include <initguid.h>",
                 "the scheduler GUIDs are emitted in-place rather than linked from -ltaskschd")
    for lib in INSTALLER_LINK_LIBS:
        if lib == "-ltaskschd":
            raise SystemExit("installer gate: -ltaskschd does not resolve under Zig's arm64 link step")
    register_shard = source("installer_register.cpp")
    uninstall_anchor = "bool gc_uninstall_execute(const WCHAR* installDirectory, char* error, size_t errorSize)"
    ctx.require_order_in_operation(register_shard, uninstall_anchor,
                                   "gc_remove_startup_tasks()",
                                   "RemoveDirectoryW(installDirectory)",
                                   "the logon tasks are removed as part of the uninstall")
    ctx.require_order_in_operation(register_shard, uninstall_anchor,
                                   "gc_remove_tray_autostart_values()",
                                   "RemoveDirectoryW(installDirectory)",
                                   "the tray Run values are removed as part of the uninstall")
    # The service reports STOPPED from inside its own process; deleting its
    # binary before that process exits is what leaves the folder behind.
    ctx.require_order_in_operation(register_shard, uninstall_anchor,
                                   "WaitForSingleObject(serviceProcess.get()",
                                   "DeleteFileW(path)",
                                   "the service process has exited before its binary is deleted")
    # The uninstaller removes its own running image so the folder can go now
    # rather than at the next restart -- but only when it IS the installed copy.
    # The same function runs inside the setup stub launched with --uninstall,
    # which normally sits in a downloads folder.
    require_text(register_shard, "gc_uninstall_self_is_installed_copy(selfPathUtf8, installDirectoryUtf8)",
                 "the setup stub never schedules itself for deletion")
    ctx.require_order_in_operation(register_shard, uninstall_anchor,
                                   "gc_delete_running_module(selfPath)",
                                   "RemoveDirectoryW(installDirectory)",
                                   "the uninstaller deletes itself before removing the folder")
    require_text(register_shard, "SetCurrentDirectoryW(system)",
                 "the working directory is moved out of the folder being removed")
    require_text(installer_util, "FileRenameInfo",
                 "the running image is unlinked by renaming its data stream")
    # Measured, not assumed: after the rename, the classic FileDispositionInfo
    # is still refused with ERROR_ACCESS_DENIED on Windows 11 26200.  Only the
    # POSIX-semantics delete unlinks the name of a mapped image.  Dropping back
    # to the classic spelling would silently restore the reboot-only behavior.
    require_text(installer_util, "FILE_DISPOSITION_FLAG_POSIX_SEMANTICS",
                 "the unlinked image is deleted with POSIX semantics, the only form that works")
    ctx.require_order_in_operation(installer_util, "bool gc_delete_running_module(const WCHAR* path)",
                                   "GC_FILE_DISPOSITION_INFO_EX_CLASS",
                                   "FileDispositionInfo,",
                                   "the classic disposition is only the fallback, never the first attempt")
    # Zig's mingw headers hide the FileDispositionInfoEx enumerator at this
    # project's _WIN32_WINNT, so the arm64 build needs the ABI value spelled out.
    require_text(installer_util, "(FILE_INFO_BY_HANDLE_CLASS)21",
                 "the POSIX disposition class survives a toolchain that hides the enumerator")

    # Anchored on the CLI parser, not the usage text: the parser is what makes
    # the verb real, and the usage text moved to main_cli_help.cpp when entry.cpp
    # reached its size ratchet.
    cli_options = source("main_cli_options.cpp")
    require_text(cli_options, "--export-active-settings", "the settings transfer export verb exists")
    require_text(cli_options, "--apply-settings-file", "the settings transfer apply verb exists")
    transfer = source("main_settings_transfer.cpp")
    # The restore is an ordinary explicit Apply.  Anything else would be a
    # second, weaker path around the event-only auto-restore contract.
    require_text(transfer, "SERVICE_APPLY_ORIGIN_CLI",
                 "restored settings are applied as an explicit CLI apply")
    require_text(transfer, "resetOcBeforeApply = true",
                 "restored settings start from stock so offsets cannot compound")
    # An APPLY request is only valid when it carries the instance id, GPU
    # generation, topology signature, and adapter from a READY envelope.  The
    # first real upgrade restore failed because this preamble was missing.
    ctx.require_order_in_operation(transfer,
                                   "static bool settings_transfer_apply(",
                                   "settings_transfer_wait_for_ready_service(",
                                   "apply_desired_settings(",
                                   "the restore establishes apply preconditions before it writes")
    require_text(transfer, "validate_configured_gpu_selection_for_client(",
                 "the restore validates the configured GPU before applying")
    require_text(transfer, "apply_ready_service_envelope_to_app(",
                 "the restore projects a READY envelope onto app state")
    forbid_text(transfer, "SERVICE_APPLY_ORIGIN_LOGON",
                "the settings transfer never impersonates a logon event")

    build_script = os.path.join(ctx.SCRIPT_DIR, "build.py")
    # The release manifest (archive root, allowlist, runtime-artifact purge) was
    # split into tools/release_manifest.py to keep build.py inside its size
    # ratchet; build.py still owns the archiving itself.
    manifest_module = os.path.join(ctx.SCRIPT_DIR, "tools", "release_manifest.py")
    require_text(manifest_module, "def release_archive_root",
                 "the archive root folder name is decided in one place")
    require_text(manifest_module, 'return "Green Curve" if os_name == "windows" else "greencurve"',
                 "Windows archives use the product folder name and Linux keeps the lowercase one")
    require_text(build_script, "installer_build.build_setup_executable",
                 "build.py produces the Windows setup executables")
