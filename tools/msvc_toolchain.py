# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""MSVC-ABI toolchain (clang-cl + lld-link) for the Windows targets.

One-way dependency, same contract as build_scheduler/zig_cache: this module
never imports build.py.  build.py passes in everything that is build-specific
(APP_VERSION, paths, the compiler-output auditor).

What this buys over llvm-mingw (verified empirically on 2026-08-28, see
llm-wiki/build.md "MSVC-ABI toolchain assessment"):

- Real OS-enforced Control Flow Guard.  The MinGW build emulates CFG with the
  module-range shim in cfg_glue.cpp; here /guard:cf produces the GFIDS table
  and the kernel validates every indirect call against it.
- Hardware shadow stacks on x64 via lld-link /cetcompat (the PE
  EX_DLLCHARACTERISTICS CET_COMPAT opt-in).  ld.lld's GNU mode has no
  equivalent, which is why llvm-mingw cannot have this.
- /GS security cookie in the PE load config and, on ARM64, CFG metadata
  (GFIDS) that the Zig aarch64 build never had.

The toolchain is a SYSTEM dependency: clang-cl/lld-link come from a standalone
LLVM install or the VS-bundled Clang component, and the headers/libraries come
from Visual Studio + the Windows SDK, none of which are redistributable.  That
breaks the hermetic self-downloading model on purpose and by explicit decision:
auto mode falls back LOUDLY to llvm-mingw when the MSVC-ABI stack is absent.
"""

import os
import subprocess
import sys

# Search order for the LLVM tools.  The standalone LLVM install is preferred:
# it ships lld-link and llvm-pdbutil alongside clang-cl, and its version tends
# to track the project's llvm-mingw closely.
_LLVM_STANDALONE_DIRS = [
    r"C:\Program Files\LLVM\bin",
    r"C:\Program Files (x86)\LLVM\bin",
]
_VSWHERE = (r"C:\Program Files (x86)\Microsoft Visual Studio"
            r"\Installer\vswhere.exe")
_PROBE_SOURCE = """\
#include <windows.h>
int main(void) {
    // One indirect call so /guard:cf has something to guard even in a probe.
    void (*local)(void) = (void (*)(void))main;
    (void)local;
    return 0;
}
"""


class MsvcToolchain:
    """Discovered MSVC-ABI tools plus the versions that produced a build."""

    def __init__(self, clang_cl, lld_link, pdbutil, label, clang_version,
                 msvc_version, sdk_version):
        self.clang_cl = clang_cl
        self.lld_link = lld_link
        self.pdbutil = pdbutil
        self.label = label
        self.clang_version = clang_version
        self.msvc_version = msvc_version
        self.sdk_version = sdk_version

    def report(self):
        print(f"MSVC-ABI toolchain: {self.label}")
        print(f"  clang-cl : {self.clang_version}")
        print(f"  MSVC     : {self.msvc_version}")
        print(f"  Win SDK  : {self.sdk_version}")


def _first_existing(paths):
    for path in paths:
        if os.path.isfile(path):
            return path
    return None


def _vs_install_roots():
    """Best-effort Visual Studio instance roots via vswhere (may be empty)."""
    if not os.path.isfile(_VSWHERE):
        return []
    try:
        result = subprocess.run(
            [_VSWHERE, "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return []
    roots = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line and os.path.isdir(line):
            roots.append(line)
    return roots


def _candidate_installations():
    """Yield candidate toolchain roots, best first."""
    # 1) Standalone LLVM installs.
    for directory in _LLVM_STANDALONE_DIRS:
        clang = os.path.join(directory, "clang-cl.exe")
        if os.path.isfile(clang):
            yield (f"standalone LLVM ({directory})",
                   {"clang_cl": clang,
                    "lld_link": os.path.join(directory, "lld-link.exe"),
                    "pdbutil": os.path.join(directory, "llvm-pdbutil.exe")})
    # 2) The Clang component inside Visual Studio.  It does not ship lld-link
    #    or llvm-pdbutil, so those fall back to whatever the LLVM root has.
    for root in _vs_install_roots():
        llvm_bin = os.path.join(root, "VC", "Tools", "Llvm", "x64", "bin")
        clang = os.path.join(llvm_bin, "clang-cl.exe")
        if os.path.isfile(clang):
            yield (f"Visual Studio bundled Clang ({llvm_bin})",
                   {"clang_cl": clang,
                    "lld_link": _first_existing(
                        [os.path.join(llvm_bin, "lld-link.exe")]
                        + [os.path.join(d, "lld-link.exe")
                           for d in _LLVM_STANDALONE_DIRS]),
                    "pdbutil": _first_existing(
                        [os.path.join(llvm_bin, "llvm-pdbutil.exe")]
                        + [os.path.join(d, "llvm-pdbutil.exe")
                           for d in _LLVM_STANDALONE_DIRS])})
    # 3) PATH.
    path_dirs = os.environ.get("PATH", "").split(os.pathsep)
    clang = _first_existing([os.path.join(d, "clang-cl.exe") for d in path_dirs])
    if clang:
        directory = os.path.dirname(clang)
        yield (f"PATH ({directory})",
               {"clang_cl": clang,
                "lld_link": _first_existing(
                    [os.path.join(directory, "lld-link.exe")]
                    + [os.path.join(d, "lld-link.exe")
                       for d in _LLVM_STANDALONE_DIRS]),
                "pdbutil": _first_existing(
                    [os.path.join(directory, "llvm-pdbutil.exe")]
                    + [os.path.join(d, "llvm-pdbutil.exe")
                       for d in _LLVM_STANDALONE_DIRS])})


def _capture(cmd, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True,
                          errors="replace", cwd=cwd)


def _probe_toolchain(tools, work_dir):
    """Fail-closed verification: compile AND link a probe with the real flag
    set for both architectures.  This proves MSVC header/library autodetection
    works for both clang-cl and lld-link before a real build depends on it."""
    problems = []
    os.makedirs(work_dir, exist_ok=True)
    source = os.path.join(work_dir, "msvc_probe.cpp")
    with open(source, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(_PROBE_SOURCE)
    for arch, target_flags, extra_link in (
            ("x64", [], ["-cetcompat"]),
            ("arm64", ["--target=aarch64-pc-windows-msvc",
                       "-mbranch-protection=standard"], [])):
        obj = os.path.join(work_dir, f"msvc_probe_{arch}.obj")
        exe = os.path.join(work_dir, f"msvc_probe_{arch}.exe")
        compiled = _capture([tools["clang_cl"], "-nologo", "-c", "-GS", "-guard:cf",
                             "-sdl", "-W4", "-WX", "-EHs-c-", "-GR-", "-Zi",
                             *target_flags, source, f"-Fo{obj}"])
        if compiled.returncode != 0 or not os.path.isfile(obj):
            problems.append(f"{arch} probe compile failed:\n{compiled.stdout}{compiled.stderr}")
            continue
        linked = _capture([tools["lld_link"], "-nologo", "-guard:cf",
                           "-opt:ref,icf", "-subsystem:console",
                           *extra_link, f"-out:{exe}", obj])
        if linked.returncode != 0 or not os.path.isfile(exe):
            problems.append(f"{arch} probe link failed:\n{linked.stdout}{linked.stderr}")
    return problems


def _discover_details(tools):
    clang_version = "unknown"
    clang = _capture([tools["clang_cl"], "--version"])
    if clang.returncode == 0 and clang.stdout:
        clang_version = clang.stdout.splitlines()[0].strip()
    msvc_version, sdk_version = _msvc_and_sdk_versions()
    return MsvcToolchain(tools["clang_cl"], tools["lld_link"], tools["pdbutil"],
                         label=tools["label"], clang_version=clang_version,
                         msvc_version=msvc_version, sdk_version=sdk_version)


def _msvc_and_sdk_versions():
    """Best-effort version reporting for the attested-build toolchain record."""
    msvc_version, sdk_version = "unknown", "unknown"
    for root in _vs_install_roots():
        msvc_dir = os.path.join(root, "VC", "Tools", "MSVC")
        if os.path.isdir(msvc_dir):
            versions = sorted(os.listdir(msvc_dir))
            if versions:
                msvc_version = versions[-1]
        kits = r"C:\Program Files (x86)\Windows Kits\10\Lib"
        if os.path.isdir(kits):
            sdk_versions = [name for name in sorted(os.listdir(kits))
                            if name.startswith("10.")]
            if sdk_versions:
                sdk_version = sdk_versions[-1]
        if msvc_version != "unknown":
            break
    return msvc_version, sdk_version


def resolve_requested(mode, target, script_dir):
    """Return an MsvcToolchain, or None when the llvm-mingw path applies.

    mode: --toolchain choice (auto / clang-cl / llvm-mingw).
    target: resolved --target (windows / linux / all).

    auto on a Windows host prefers clang-cl and falls back LOUDLY; forcing
    clang-cl fails the build instead of silently degrading; Linux hosts and
    Linux-only targets always return None (clang-cl cannot target Linux).
    """
    if mode == "llvm-mingw":
        return None
    windows_needed = target in ("windows", "all")
    if sys.platform != "win32" or not windows_needed:
        if mode == "clang-cl":
            print("ERROR: --toolchain clang-cl requires a native Windows host "
                  "and a Windows target (the MSVC ABI cannot be cross-built "
                  "from Linux)")
            sys.exit(1)
        return None
    for label, tools in _candidate_installations():
        if not (tools["clang_cl"] and tools["lld_link"]):
            continue
        details = _discover_details({**tools, "label": label})
        problems = _probe_toolchain(tools, os.path.join(script_dir,
                                                        "build-tmp", "msvc-probe"))
        if problems:
            print(f"MSVC-ABI candidate failed verification: {label}")
            for problem in problems:
                print(f"  {problem}")
            continue
        return details
    if mode == "clang-cl":
        print("ERROR: --toolchain clang-cl requested but no verified "
              "clang-cl + lld-link installation was found.  Install standalone "
              f"LLVM under {' or '.join(_LLVM_STANDALONE_DIRS)} (with lld-link), "
              "or Visual Studio with the C++ Clang component, or file the tools "
              "into PATH.")
        sys.exit(1)
    print("MSVC-ABI toolchain not found or verification failed; "
          "falling back to llvm-mingw (install standalone LLVM plus Visual "
          "Studio Build Tools to build the hardened MSVC-ABI artifacts)")
    return None


# ---------------------------------------------------------------------------
# Hardened flag sets (cl-style spellings; clang-cl rejects GNU-style flags
# under -WX, and -Oz is not available — /O1 is the size-oriented floor).
# ---------------------------------------------------------------------------

def windows_compile_flags(service, arch, app_version, build_number, source_dir,
                          debug=True):
    """Compile flags for one Windows translation unit (MSVC ABI)."""
    flags = [
        "-nologo", "-std:c++17", "-O1", "-DNDEBUG", "-c",
        f'-DAPP_VERSION="{app_version}"',
        f"-DAPP_BUILD_NUMBER={build_number}",
        # MSVC's ucrt headers deprecate the C99 stdio functions this project
        # deliberately uses (see installer_util.cpp); the MinGW build never
        # sees these warnings, so silence them for warning-as-error parity.
        "-D_CRT_SECURE_NO_WARNINGS",
        f"-I{source_dir}",
        "-GS", "-guard:cf", "-sdl",
        "-W4", "-WX", "-Wno-unused-function", "-Wno-unused-parameter",
        # Match the llvm-mingw build's -fno-exceptions -fno-rtti.
        "-EHs-c-", "-GR-",
        # Hardening flags clang-cl accepts cleanly (verified under -WX).
        "-ftrivial-auto-var-init=pattern",
        "-fno-delete-null-pointer-checks",
    ]
    if debug:
        flags.append("-Zi")
    if service:
        flags.append("-DGREEN_CURVE_SERVICE_BINARY=1")
    if arch == "arm64":
        flags += ["--target=aarch64-pc-windows-msvc",
                  "-mbranch-protection=standard"]
    return flags


def windows_link_flags(pdb_name, arch, debug=True):
    """Link flags for one Windows binary (lld-link, MSVC ABI)."""
    flags = ["-nologo", "-guard:cf", "-opt:ref,icf", "-subsystem:windows"]
    if arch == "x64":
        # Shadow-stack (CET backward edge) opt-in.  x64-only: there is no
        # CETCOMPAT for ARM64; arm64 gets CFG metadata + PAC/BTI instead.
        flags.append("-cetcompat")
    if debug:
        flags += ["-debug:full", f"-pdb:{pdb_name}"]
    return flags


def msvc_link_libs(mingw_libs):
    """Translate the shared -l<name> library lists to MSVC <name>.lib form."""
    out = []
    for lib in mingw_libs:
        if not lib.startswith("-l") or len(lib) <= 2:
            raise ValueError(f"unexpected link library entry for MSVC: {lib!r}")
        out.append(lib[2:] + ".lib")
    return out


# ---------------------------------------------------------------------------
# Compile/link execution (mirror of the llvm-mingw object-first flow).
# ---------------------------------------------------------------------------

def compile_windows_objects(clang_cl, compile_flags, sources, object_dir,
                            run_compiler, jobs=1, limiter=None):
    """Compile the Windows TUs to objects in parallel (scheduler-managed)."""
    from build_scheduler import compile_objects  # one-way dependency
    os.makedirs(object_dir, exist_ok=True)
    compiles = []
    objects = []
    for index, source in enumerate(sources):
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(object_dir, f"{index:02d}-{stem}.obj")
        compiles.append((source, [clang_cl, *compile_flags, source, f"-Fo{obj}"],
                         os.getcwd()))
        objects.append(obj)
    compile_objects(compiles, run_compiler, jobs, limiter)
    return objects


def link_windows(lld_link, link_flags, objects, res_path, libs, output_path,
                 run_compiler):
    """Link one Windows binary from objects + the compiled resource script."""
    cmd = [lld_link, *link_flags, f"-out:{output_path}",
           *objects, res_path, *libs]
    return run_compiler(cmd, allow_cfg_collision=True)


# ---------------------------------------------------------------------------
# Deterministic self-tests (pure; no compiler required).
# ---------------------------------------------------------------------------

def run_self_tests():
    compile_flags = windows_compile_flags(
        service=True, arch="arm64", app_version="1.2", build_number=7,
        source_dir=r"C:\src\source")
    for required in ("-c", "-D_CRT_SECURE_NO_WARNINGS", "-GS", "-guard:cf",
                     "-sdl", "-EHs-c-", "-GR-",
                     "-W4", "-WX", "-ftrivial-auto-var-init=pattern",
                     "-DGREEN_CURVE_SERVICE_BINARY=1",
                     "--target=aarch64-pc-windows-msvc",
                     "-mbranch-protection=standard", "-Zi"):
        if required not in compile_flags:
            raise AssertionError(f"arm64 service compile flags missing {required}")
    if any(flag.startswith("-Wl,") or flag.startswith("-l") for flag in compile_flags):
        raise AssertionError("GNU-style linker flags leaked into MSVC compile flags")
    x64_flags = windows_compile_flags(service=False, arch="x64",
                                      app_version="1.2", build_number=7,
                                      source_dir="source", debug=False)
    if "-Zi" in x64_flags or "-DGREEN_CURVE_SERVICE_BINARY=1" in x64_flags:
        raise AssertionError("debug/service flags leaked into plain x64 flags")
    x64_link = windows_link_flags("greencurve.pdb", "x64", debug=False)
    if "-cetcompat" not in x64_link:
        raise AssertionError("x64 link must opt into CET shadow stacks")
    if "-debug:full" in x64_link:
        raise AssertionError("debug=False must omit PDB emission")
    a64_link = windows_link_flags("greencurve.pdb", "arm64")
    if "-cetcompat" in a64_link:
        raise AssertionError("cetcompat is x64-only and must not reach arm64 links")
    if msvc_link_libs(["-luser32", "-lbcrypt"]) != ["user32.lib", "bcrypt.lib"]:
        raise AssertionError("-l library mapping produced the wrong .lib names")
    for bad in ("-static", "-l", "foo.lib"):
        try:
            msvc_link_libs([bad])
        except ValueError:
            continue
        raise AssertionError(f"non -l entry {bad!r} must be refused")
    print("msvc_toolchain self-tests passed")
