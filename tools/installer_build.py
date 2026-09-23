"""Build the Green Curve setup executable and its uninstaller.

Split out of build.py for the same reason security_gates.py and ui_gates.py
were: build.py is under a size ratchet that may only shrink.  The dependency is
one-way — this module never imports build.py; the caller passes its own module
in as `ctx` so paths, flags, and the current version/build number all come from
one place.

What a setup file is
--------------------

    [ installer stub PE ][ stored GCAR container ][ 44-byte footer ]

The stub is an ordinary hardened Windows executable built from source/installer_*.
The container holds the exact release manifest plus uninstall.exe.  The footer
is read from the end of the file at runtime; the format, and every bounds check
the installer performs on it, live in source/installer_archive_policy.h so
`build.py --test` covers them.

Compression (deliberately none)
-------------------------------

The container is STORED.  An unsigned executable whose tail is ~1.2 MB of
entropy-8.0 data in an unknown format is the textbook shape of a packed
dropper, and antivirus heuristics score it that way; stored, the overlay is the
same plain PE/text bytes the .7z ships, which every scanner can inspect.  The
cost is roughly 0.9 MB of download.  It also makes Windows- and Linux-hosted
setup files identical in layout (Linux could never compress).

The installer still ACCEPTS the XPRESS_HUFF method (source/installer_payload.cpp,
cabinet.dll's Compression API) so the file format is unchanged; the build just
never produces it.  `_verify_setup_file` refuses anything but STORE.
"""

import os
import shutil
import struct
import subprocess

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

ARCHIVE_FLAG_NONE = 0
ARCHIVE_FLAG_UNINSTALLER = 1

INSTALLER_SOURCE_NAMES = [
    "installer_main.cpp",
    "installer_ui.cpp",
    "installer_ui_pages.cpp",
    "installer_theme.cpp",
    "installer_apply.cpp",
    "installer_move_cleanup.cpp",
    "installer_register.cpp",
    "installer_autostart.cpp",
    "installer_payload.cpp",
    "installer_util.cpp",
    "service_acl.cpp",
    "service_path_chain.cpp",
    "service_acl_handle.cpp",
    "service_install_location.cpp",
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
            VALUE "CompanyName", "COMPANY_NAME"
            VALUE "FileDescription", "DESCRIPTION"
            VALUE "FileVersion", "VER_STR"
            VALUE "InternalName", "INTERNAL_NAME"
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


def installer_original_filename(ctx, arch, uninstaller):
    """The name each installer-family PE ships under, which its VERSIONINFO
    must repeat exactly (a mismatch reads as a renamed binary to heuristics)."""
    if uninstaller:
        return "uninstall.exe"
    return f"greencurve-{ctx.APP_VERSION}-windows-{arch}-setup.exe"


def _write_installer_resources(ctx, work, uninstaller, arch):
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
          .replace("COMPANY_NAME", build_state.VERSION_COMPANY_NAME)
          .replace("INTERNAL_NAME", "GreenCurveUninstall" if uninstaller else "GreenCurveSetup")
          .replace("DESCRIPTION", "Green Curve uninstaller" if uninstaller else "Green Curve setup")
          .replace("ORIGINAL_NAME", installer_original_filename(ctx, arch, uninstaller)))
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
    res_path = _write_installer_resources(ctx, work, uninstaller, arch)

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
    # Stored, never compressed: see "Compression (deliberately none)" above.
    method, blob = METHOD_STORE, container
    print(f"  installer: payload {len(container):,} bytes (stored)")
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
    if method != METHOD_STORE:
        raise RuntimeError(f"setup file payload method is {method}; release setup files are stored")
    if blob != container:
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
        for binary, is_uninstaller in ((uninstaller_path, True), (stub_path, False)):
            ctx.pe_verify.stamp_pe_checksum(binary)
            ctx.verify_release_binary(binary, "windows", arch, original_filename=
                                      installer_original_filename(ctx, arch, is_uninstaller))

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
        # The image checksum covers the overlay, so the stub's own stamp is
        # stale once the payload is appended; restamp the finished file.
        ctx.pe_verify.stamp_pe_checksum(output)
        with open(output, "rb") as handle:
            ctx.pe_verify.verify_pe_checksum(handle.read(), os.path.basename(output))
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
    require_text(apply_shard, "gc_settings_capture_attempt_result(",
                 "setup distinguishes an empty active state from an export failure")
    require_text(source("installer_ui.cpp"), "GC_SETTINGS_CAPTURE_FAILED",
                 "the completion page reports an unconfirmed settings capture")
    require_text(source("entry.cpp"), "GC_SETTINGS_TRANSFER_NO_ACTIVE_EXIT_CODE",
                 "the CLI exposes the confirmed no-active result to setup")
    require_text(source("gui_update_settings_handoff.cpp"),
                 "return noActive ? GC_SETTINGS_CAPTURE_NONE_ACTIVE :",
                 "the updater distinguishes an empty active state from capture failure")
    require_text(source("gui_update_dialog.cpp"),
                 "capture == GC_SETTINGS_CAPTURE_FAILED",
                 "the updater warns before an update that cannot preserve settings")
    require_text(source("installer_main.cpp"),
                 "context.settingsCaptureHandledByGui = options->settingsCaptureHandledByGui",
                 "the updater setup child does not retry an unauthorized session-0 capture")
    # Ordering is the correctness property of an upgrade: capture the live
    # settings while the old build is still running, then stop it, then replace
    # its files.  Anchored to gc_install_execute() so the checks read call order
    # inside the one function that owns it, not the order the helpers happen to
    # be defined in further up the file.
    install_anchor = "bool gc_install_execute(GcInstallContext* context)"
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "classify_path_protection(targetDirectory, &preProtection, true)",
                                   "gc_capture_active_settings(context);",
                                   "the path-protection preflight runs before setup disturbs the live installation")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_capture_active_settings(context);",
                                   "gc_stop_gui_processes(context)",
                                   "settings are captured before anything is stopped")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "transaction.prepare(targetDirectory)",
                                   "gc_stop_service(context, &serviceWasRunning)",
                                   "the previous files and registration are saved before shutdown")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_stop_service(context, &serviceWasRunning)",
                                   "transaction.replace(i)",
                                   "the service is stopped before its binary is replaced")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_register_service(context)",
                                   "gc_reapply_captured_settings(context)",
                                   "settings are re-applied only after the new service is registered")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_write_uninstall_registration(context)",
                                   "gc_register_service(context)",
                                   "the uninstall record is recoverable before the service helper commits")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "transaction.cleanup();",
                                   "gc_update_shortcuts(context)",
                                   "shortcuts are changed only after service registration commits")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "gc_update_shortcuts(context)",
                                   "gc_retire_previous_directory(context)",
                                   "the old directory is retired only after the new install is registered")
    # The pinning handle holds the target without FILE_SHARE_DELETE, so the
    # created-folder guard must be declared first (destroyed after it closes).
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "} createdTarget = {targetDirectory,",
                                   "GcScopedHandle targetHandle(",
                                   "a failed fresh install removes the folders it created")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "transaction.cleanup();\n    createdTarget.armed = false;",
                                   "gc_update_shortcuts(context)",
                                   "a committed install keeps its folder")
    forbid_text(source("installer_transaction_files.h"), "SHFileOperation",
                "created-folder cleanup never deletes recursively")
    require_text(source("installer_transaction_files.h"), "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH",
                 "payload files are replaced atomically")
    require_text(source("installer_transaction.cpp"), "restore_files()",
                 "every failed replacement can restore the previous payload")
    require_text(source("installer_transaction.cpp"), "restore_service()",
                 "a failed registration restores the previous SCM configuration")
    require_text(source("installer_transaction.cpp"), "restore_registry()",
                 "a failed registration restores the uninstall record")
    require_text(source("installer_transaction_files.h"), "CopyFile2(staged, temporary, nullptr)",
                 "staged payload copies inherit target permissions instead of the protected scratch ACL")
    forbid_text(source("installer_transaction_files.h"), "CopyFileW(staged, temporary",
                "CopyFileW would copy the scratch ACL and block standard users from the installed GUI")
    require_text(apply_shard, "--service-install",
                 "the installed binary owns its own SCM registration")
    require_text(apply_shard, "gc_install_paths_equal(registeredUtf8, context->plan.targetDirectory)",
                 "the re-pointed service registration is verified against the new directory")
    cleanup_shard = source("installer_move_cleanup.cpp")
    require_text(cleanup_shard, "gc_install_input_paths_overlap_or_unresolved(previous, target)",
                 "move cleanup preserves an old directory traversed through a mount point")
    require_text(cleanup_shard, "gc_cleanup_directories_overlap(oldIdentity, newIdentity)",
                 "move cleanup cannot alter an ancestor of the new service path")
    require_text(cleanup_shard, "context->plan.cleanupPreviousDirectory && location == GC_SVC_LOCATION_OK",
                 "only an ordinary setup-managed old folder may have owned files removed")
    require_text(cleanup_shard, "release_service_hardening(previous, GC_SERVICE_ACL_DIRECTORY",
                 "a retained moved-from directory is released (only if provably ours, "
                 "never to a null DACL) so the user can delete it")
    require_text(source("installer_move_cleanup.h"), "RemoveDirectoryW(directory)",
                 "move cleanup removes the old directory only when empty")
    require_text(source("installer_launch.cpp"), "CreateProcessWithTokenW",
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
    # The old "direct child of Program Files" gate is gone on purpose: the
    # administrator picks the folder, the classification states how well it can
    # be protected, and the acknowledgment is the consent.  What must NOT
    # return is a silent install past a non-protected location, or a silent
    # downgrade between the preflight verdict and the hardened directory.
    require_text(apply_shard, "gc_path_protection_requires_acknowledgment",
                 "an interactive install cannot proceed past a non-protected path without acknowledgment")
    require_text(apply_shard, "GC_PATH_PROTECTION_ACKNOWLEDGMENT_LABEL",
                 "the refusal names the acknowledgment the user must tick")
    require_text(apply_shard, "no longer as protected as it was when setup",
                 "a location that lost its protection between preflight and hardening fails closed")
    require_text(apply_shard, "skipping directory DACL",
                 "a filesystem without ACLs is an explicit logged capability gap, never a silent skip")
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
                                   "gc_refresh_folder_protection(wizard, chosenWide)",
                                   "StringCchCopyA(wizard->options.directory",
                                   "the folder page classifies the service root before accepting the choice")
    require_text(installer_ui, "GC_ID_RISK_ACCEPT",
                 "the folder page offers the explicit path-risk acknowledgment")
    # The wizard must hand gc_install_execute the user's ACTUAL answer.  An
    # unconditional `pathRiskAcknowledged = true` made the apply-side check --
    # the second line of defence pinned above -- inert on every GUI run while
    # every gate still passed.
    require_text(installer_ui, "pathRiskAcknowledged = wizard->riskAccepted",
                 "the installer-side acknowledgment check sees the real answer")
    forbid_text(installer_ui, "pathRiskAcknowledged = true",
                "the folder page never fakes the path-risk acknowledgment")
    require_text(apply_shard, "apply_protected_service_dacl_to_handle(targetHandle.get()",
                 "the install directory is hardened through its pinned handle before extraction")
    ctx.require_order_in_operation(apply_shard, install_anchor,
                                   "apply_protected_service_dacl_to_handle(targetHandle.get()",
                                   "gc_capture_active_settings(context);",
                                   "the folder is judged and hardened before the live install is disturbed")
    require_text(apply_shard, "gc_service_install_location_verdict_for_handle(targetDirectory",
                 "setup re-judges the pinned folder, not a fresh lookup by name")
    installer_util = source("installer_util.cpp")
    require_text(installer_util, "FOLDERID_ProgramFiles",
                 "elevated capture helpers stage beneath an administrator-owned parent")
    # The chain proof replaces the direct-child-of-Program-Files proxy; its
    # fail-safe direction and its consent wording are the security contract.
    path_chain_cpp = source("service_path_chain.cpp")
    path_chain_policy = source("service_path_chain_policy.h")
    require_text(path_chain_cpp, "scan_dacl_for_danger",
                 "the path proof scans every component for substitution-capable rights")
    require_text(path_chain_cpp, "INHERIT_ONLY",
                 "the chain walk separates object rights from inherit-only ones")
    require_text(path_chain_cpp, "FILE_FLAG_OPEN_REPARSE_POINT",
                 "the chain walk never follows a reparse point while inspecting it")
    require_text(path_chain_policy, "GC_PATH_RISK_ESCALATION_SENTENCE",
                 "the consent wording carries the SYSTEM-escalation sentence")
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
    uninstall_anchor = "bool gc_uninstall_execute(const WCHAR* installDirectory, bool* folderLeftForRestart,"
    with open(register_shard, "r", encoding="utf-8", errors="replace") as handle:
        uninstall_text = handle.read().split(uninstall_anchor, 1)[1]
    if "gc_inspect_service_before_uninstall(&runningProcess, &serviceRegistered," not in uninstall_text:
        raise RuntimeError("uninstall must establish service state before deleting files")
    helper_failure = uninstall_text.split("if (!gc_run_and_wait(guiPath, commandLine", 1)[1].split(
        'gc_log_step("uninstall: background service removed")', 1)[0]
    if "return false;" not in helper_failure:
        raise RuntimeError("uninstall must preserve files when service removal fails")
    missing_gui = uninstall_text.split("} else if (serviceRegistered) {", 1)[1].split(
        "gc_remove_shortcut(", 1)[0]
    if "return false;" not in missing_gui:
        raise RuntimeError("uninstall must preserve files if its helper is missing and service exists")
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
    # The uninstaller schedules its own running image (and so the folder) for
    # the next restart -- but only when it IS the installed copy.  The same
    # function runs inside the setup stub launched with --uninstall, which
    # normally sits in a downloads folder.
    require_text(register_shard, "gc_uninstall_self_is_installed_copy(selfPathUtf8, installDirectoryUtf8)",
                 "the setup stub never schedules itself for deletion")
    ctx.require_order_in_operation(register_shard, uninstall_anchor,
                                   "MoveFileExW(selfPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)",
                                   "RemoveDirectoryW(installDirectory)",
                                   "the uninstaller hands itself to the session manager before removing the folder")
    require_text(register_shard, "SetCurrentDirectoryW(system)",
                 "the working directory is moved out of the folder being removed")
    # Antivirus hygiene: deleting a RUNNING image in place (rename its data
    # stream into an alternate stream, then a POSIX-semantics delete) is a
    # published malware self-deletion technique that behavior monitors flag.
    # It must not appear in any binary we ship.
    for name in sorted(os.listdir(ctx.SOURCE_DIR)):
        if not name.endswith((".cpp", ".h")):
            continue
        for needle in ("FileRenameInfo", "FILE_DISPOSITION_FLAG_POSIX_SEMANTICS",
                       "FileDispositionInfoEx", "(FILE_INFO_BY_HANDLE_CLASS)21"):
            forbid_text(source(name), needle,
                        f"source/{name} must not rename/unlink a running image ({needle})")
    # The finish page must not claim the folder is gone when it is pending.
    require_text(source("installer_ui.cpp"), "is deleted at the next restart",
                 "the uninstall finish page reports a restart-pending folder")

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
