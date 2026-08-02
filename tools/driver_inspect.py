"""Hardware-free NVIDIA driver inspection.

Answers "would Green Curve work on this driver?" by reading an extracted driver
tree directly -- no GPU, and no matching host architecture, required.  Two
entry points:

  inspect_aarch64_driver()        Linux aarch64 (.run --extract-only tree)
  inspect_arm64_windows_driver()  Windows on Arm (7-Zip-extracted setup .exe)

Split out of build.py so the build script stays under its size ratchet; build.py
owns the CLI and passes its toolchain paths in through `ctx`.  Nothing here
imports build.py, so the dependency runs one way only.

`ctx` is any object exposing LLVM_MINGW_DIR.
"""
import os
import shutil
import struct
import subprocess


# The NvAPI entry points Green Curve resolves through nvapi_QueryInterface.
# These are NOT exported symbols -- nvapi_QueryInterface is the only export, and
# everything else comes back by 32-bit id from an internal dispatch table.  An
# export listing therefore cannot tell you whether the OC surface exists on a
# given build; scanning for the ids as little-endian literals can.
#
# Kept in sync with source/gpu_core.h (public ids) and source/vf_backends.cpp
# (the private pstates20 VF ids); a build gate pins the values.
NVAPI_ENTRY_POINT_IDS = [
    ("NvAPI_Initialize", 0x0150E828, False),
    ("NvAPI_EnumPhysicalGPUs", 0xE5AC921F, False),
    ("NvAPI_GPU_GetFullName", 0xCEEE8E9F, False),
    ("NvAPI_GPU_GetPCIIdentifiers", 0x2DDFB66E, False),
    ("NvAPI_GPU_GetArchInfo", 0xD8265D24, False),
    ("NvAPI_GPU_GetBusId", 0x1BE0B8E5, False),
    ("NvAPI_GPU_GetBusSlotId", 0x2A0A350F, False),
    # The private VF-curve surface: the whole point of the scan.
    ("VF getPstates20 (status)", 0x21537AD4, True),
    ("VF getPstates20Info", 0x507B4B59, True),
    ("VF getControl", 0x23F1B133, True),
    ("VF setControl", 0x0733E009, True),
]


def _scan_dword_ids(path, ids):
    """Count little-endian 4-byte occurrences of each id in a file.

    A specific 4-byte value appears by chance roughly once per 4 billion bytes,
    so in an ~800 KB image a hit is ~0.02% likely at random.  Presence is strong
    evidence the id is a real dispatch-table entry; absence across the whole set
    is strong evidence the surface is not in this build.  Neither is proof the
    function works -- only hardware settles that."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return None
    counts = {}
    for name, value, _private in ids:
        counts[name] = data.count(struct.pack("<I", value))
    return counts


def scan_nvapi_entry_point_ids(path):
    """Scan the NvAPI images in `path` for the entry-point ids we resolve, and
    print them per architecture.  Images of DIFFERENT architectures from the
    SAME package are each other's control: an id present in the x64 image but
    missing from the arm64 one means the surface was stripped for arm64, not
    that our id is stale.

    Both the shim and its `_impl` sibling are scanned, and the best result per
    architecture wins.  The dispatch table lives in the *_impl* DLL -- nvapi64.dll
    is an ~800 KB shim over a ~6 MB nvapi64_impl.dll.  Skipping _impl made every
    id read MISSING in EVERY architecture including x64, which Green Curve
    resolves successfully every day; that impossible result is what exposed the
    filter as the bug rather than the driver."""
    images = []
    for root, _dirs, files in os.walk(path):
        for fn in sorted(files):
            low = fn.lower()
            if low.startswith("nvapi") and low.endswith(".dll"):
                images.append(os.path.join(root, fn))
    if not images:
        print("  no NvAPI images found to scan")
        return {}

    # Keep, per architecture, the image that resolved the most ids.
    best = {}
    for image in images:
        machine, _exports = _pe_read_exports(image)
        arch = {0x8664: "x64", 0xAA64: "arm64", 0x14C: "x86"}.get(machine, "?")
        counts = _scan_dword_ids(image, NVAPI_ENTRY_POINT_IDS) or {}
        hits = sum(1 for n, _v, _p in NVAPI_ENTRY_POINT_IDS if counts.get(n))
        if arch not in best or hits > best[arch][0]:
            best[arch] = (hits, image, counts)

    order = sorted(best, key=lambda a: (a != "arm64", a))
    header = "  {:<28}".format("entry point")
    for arch in order:
        header += "  {:>12}".format(arch)
    print(header)
    print("  " + "-" * (28 + 14 * len(order)))
    for name, _value, private in NVAPI_ENTRY_POINT_IDS:
        row = "  {:<28}".format(name + (" *" if private else ""))
        for arch in order:
            hit = best[arch][2].get(name, 0)
            row += "  {:>12}".format(f"{hit}x" if hit else "MISSING")
        print(row)
    print("  (* = private VF-curve surface; counts are literal id occurrences)")
    for arch in order:
        print(f"  {arch:>6}: {os.path.basename(best[arch][1])}")
    return {arch: best[arch][2] for arch in order}


def check_source_gates(ctx, require_text):
    """Source gates for driver inspection.  Lives here rather than in build.py so
    the build script stays under its size ratchet; build.py passes its own
    require_text in, and this module never imports build.py."""
    me = os.path.join(ctx.SCRIPT_DIR, "tools", "driver_inspect.py")
    vf_backends = os.path.join(ctx.SOURCE_DIR, "vf_backends.cpp")
    require_text(me, "def inspect_arm64_windows_driver",
                 "Windows-on-Arm driver inspection works without arm64 hardware")
    require_text(me, "def _pe_read_exports",
                 "PE export parsing backs the Windows arm64 driver inspector")
    require_text(me, "def inspect_aarch64_driver",
                 "Linux aarch64 driver inspection stayed available after the split")
    require_text(me, "nvapia64.dll",
                 "the inspector looks for the native arm64 NVAPI image")
    # The id scan is only meaningful while its table matches what the backend
    # actually passes nvapi_QueryInterface, so pin every private VF id against
    # the backend's own constants.  It must also keep scanning the *_impl
    # images: the dispatch table lives there, and excluding them made every id
    # read MISSING in EVERY architecture including the x64 one Green Curve
    # resolves successfully every day -- an impossible result, which is what
    # exposed the filter as the bug rather than the driver.
    require_text(me, "def scan_nvapi_entry_point_ids",
                 "the inspector can scan for private NvAPI entry-point ids")
    require_text(me, "_impl",
                 "the id scan covers the *_impl images holding the dispatch table")
    for entry_id in ("0x21537AD4", "0x507B4B59", "0x23F1B133", "0x0733E009"):
        require_text(vf_backends, entry_id + "u",
                     f"VF id {entry_id} is a backend constant")
        require_text(me, entry_id, f"the id scan looks for VF id {entry_id}")
    require_text(me, "0x0150E828",
                 "the id scan covers NvAPI_Initialize (matches NVAPI_INIT_ID)")


def inspect_aarch64_driver(ctx, path):
    """Verify, WITHOUT arm64 hardware, that an aarch64 NVIDIA driver ships the
    libraries + symbols our backend needs.  `path` is an extracted
    NVIDIA-Linux-aarch64-<ver>.run directory (run `./<run> --extract-only` first)
    or a directory containing the .so files.  Reads aarch64 ELF exports with the
    bundled llvm-nm."""
    print(f"=== Inspecting aarch64 driver tree: {path} ===")
    if not os.path.isdir(path):
        print(f"ERROR: not a directory: {path}")
        print("Extract the driver first:  ./NVIDIA-Linux-aarch64-<ver>.run --extract-only")
        return 1

    wanted = {
        "NVML (libnvidia-ml.so)": (
            "libnvidia-ml.so",
            ["nvmlInit_v2", "nvmlDeviceGetCount_v2", "nvmlDeviceSetClockOffsets",
             "nvmlDeviceSetGpuLockedClocks", "nvmlDeviceGetGpcClkMinMaxVfOffset",
             "nvmlDeviceSetFanSpeed_v2", "nvmlDeviceSetPowerManagementLimit"]),
        "NvAPI (libnvidia-api.so)": (
            "libnvidia-api.so",
            ["nvapi_QueryInterface"]),
    }
    nm = ctx.LLVM_MINGW_NM
    if not os.path.exists(nm):
        nm = shutil.which("llvm-nm") or shutil.which("nm")

    found = {}   # label -> bool (library present AND all required symbols exported)
    for label, (prefix, symbols) in wanted.items():
        match = None
        for root, _dirs, files in os.walk(path):
            for fn in sorted(files):
                if fn.startswith(prefix) and ".so" in fn:
                    match = os.path.join(root, fn)
                    break
            if match:
                break
        if not match:
            print(f"  {label}: NOT FOUND")
            found[label] = False
            continue
        print(f"  {label}: {os.path.relpath(match, path)}")
        if not nm:
            print("    (no llvm-nm/nm available; cannot verify exported symbols)")
            found[label] = True  # present, symbols unverified
            continue
        try:
            exported = subprocess.run([nm, "-D", "--defined-only", match],
                                      capture_output=True, text=True, errors="ignore").stdout
        except OSError:
            print("    (symbol read failed)")
            found[label] = True
            continue
        all_syms = True
        for sym in symbols:
            present = sym in exported
            all_syms = all_syms and present
            print(f"      {'OK     ' if present else 'MISSING'} {sym}")
        found[label] = all_syms

    have_nvml = found.get("NVML (libnvidia-ml.so)", False)
    have_nvapi = found.get("NvAPI (libnvidia-api.so)", False)
    if not have_nvml:
        verdict = "NVML MISSING — this driver cannot drive the GPU (not a usable target)."
    elif have_nvapi:
        verdict = "FULL — NVML + NvAPI present: VF-curve OC/UV expected to work on this driver."
    else:
        verdict = ("NVML-ONLY — NvAPI absent: clock offsets / power / fan / locked clocks work, "
                   "but no VF-curve editing on this driver version.")
    print("\nVerdict:", verdict)
    return 0 if have_nvml else 1


def _pe_read_exports(path):
    """Parse a PE/COFF image's export-name table.  Returns (machine, [names]) or
    (None, []) if `path` is not a PE.  Written against the file format rather
    than shelling out so this works on any host and needs no llvm-nm."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return None, []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None, []
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 > len(data) or data[pe_off:pe_off + 4] != b"PE\x00\x00":
        return None, []
    machine, num_sections = struct.unpack_from("<HH", data, pe_off + 4)
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    if opt_off + 2 > len(data):
        return machine, []
    magic = struct.unpack_from("<H", data, opt_off)[0]
    # Export directory is DataDirectory[0]; it sits at a different offset in
    # PE32 (0x60) and PE32+ (0x70).
    dd_off = opt_off + (0x70 if magic == 0x20B else 0x60)
    if dd_off + 8 > len(data):
        return machine, []
    export_rva, _export_size = struct.unpack_from("<II", data, dd_off)
    if export_rva == 0:
        return machine, []

    sections = []
    sec_off = opt_off + opt_size
    for i in range(num_sections):
        entry = sec_off + i * 40
        if entry + 40 > len(data):
            break
        virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, entry + 8)
        sections.append((virt_addr, max(virt_size, raw_size), raw_ptr))

    def rva_to_off(rva):
        for virt_addr, size, raw_ptr in sections:
            if virt_addr <= rva < virt_addr + size:
                return raw_ptr + (rva - virt_addr)
        return None

    dir_off = rva_to_off(export_rva)
    if dir_off is None or dir_off + 40 > len(data):
        return machine, []
    num_names = struct.unpack_from("<I", data, dir_off + 24)[0]
    names_rva = struct.unpack_from("<I", data, dir_off + 32)[0]
    names_off = rva_to_off(names_rva)
    if names_off is None:
        return machine, []

    names = []
    for i in range(num_names):
        entry = names_off + i * 4
        if entry + 4 > len(data):
            break
        name_off = rva_to_off(struct.unpack_from("<I", data, entry)[0])
        if name_off is None or name_off >= len(data):
            continue
        end = data.find(b"\x00", name_off)
        if end < 0:
            continue
        names.append(data[name_off:end].decode("ascii", "ignore"))
    return machine, names


def inspect_arm64_windows_driver(ctx, path):
    """Verify, WITHOUT arm64 hardware, that a Windows-on-Arm NVIDIA driver ships
    the DLLs + entry points our backend needs.  `path` is a directory holding an
    extracted driver package -- NVIDIA's setup .exe is a 7-Zip-openable archive:

        7z x 616.00_DeveloperPreview_win11_arm64_International.exe -o<dir>

    Answers the question the RTX Spark developer preview left open: whether an
    arm64 GeForce driver exposes NVAPI at all.  A missing nvapia64.dll is
    decisive (no VF-curve control is possible); a present one is necessary but
    NOT sufficient, because the private pstates20 entry points are resolved by
    ordinal through nvapi_QueryInterface and cannot be proven without hardware."""
    print(f"=== Inspecting Windows arm64 driver tree: {path} ===")
    if not os.path.isdir(path):
        print(f"ERROR: not a directory: {path}")
        print("Extract the driver package first, e.g.:")
        print("  7z x <driver>_win11_arm64_International.exe -oarm64-driver")
        return 1

    # NVIDIA names the NVAPI runtime per machine architecture and ships every
    # variant side by side in a Windows-on-Arm package: nvapia64.dll is ARM64,
    # nvapi64.dll is the x64 emulation copy, nvapi.dll the x86 one.  Looking for
    # "nvapi64.dll" therefore finds the WRONG image on arm64 -- which is exactly
    # the bug this inspector caught in Green Curve's own loader.  NVML keeps the
    # plain name for the native image (nvml_arm64ec.dll is the compat copy).
    wanted = {
        "NVAPI (nvapia64.dll)": ("nvapia64.dll", ["nvapi_QueryInterface"]),
        "NVML (nvml.dll)": ("nvml.dll", [
            "nvmlInit_v2", "nvmlDeviceGetCount_v2", "nvmlDeviceSetClockOffsets",
            "nvmlDeviceSetGpuLockedClocks", "nvmlDeviceGetGpcClkMinMaxVfOffset",
            "nvmlDeviceGetMemClkMinMaxVfOffset", "nvmlDeviceGetNumFans",
            "nvmlDeviceSetFanSpeed_v2", "nvmlDeviceSetPowerManagementLimit",
            "nvmlDeviceGetPowerManagementLimitConstraints"]),
    }

    # Presence, architecture and symbol completeness are tracked separately: a
    # DLL that is present with every symbol but built for x64 is a different
    # (and much more informative) answer than a missing one.
    found = {}
    for label, (filename, symbols) in wanted.items():
        match = None
        for root, _dirs, files in os.walk(path):
            for fn in sorted(files):
                if fn.lower() == filename:
                    match = os.path.join(root, fn)
                    break
            if match:
                break
        if not match:
            print(f"  {label}: NOT FOUND")
            found[label] = {"present": False, "arch_ok": False, "syms_ok": False, "arch": "-"}
            continue

        machine, exports = _pe_read_exports(match)
        arch = {0x8664: "x64", 0xAA64: "arm64", 0x14C: "x86"}.get(
            machine, f"0x{machine:04X}" if machine else "not a PE")
        arch_ok = machine == 0xAA64
        print(f"  {label}: {os.path.relpath(match, path)}  [{arch}]")
        if not arch_ok:
            # An x64 DLL inside an arm64 package is the emulation/cross-compile
            # copy, not something a native arm64 binary can load.
            print(f"    WARNING: expected an arm64 (0xAA64) image, got {arch}")
        if not exports:
            print("    (no export table readable; cannot verify symbols)")
            found[label] = {"present": True, "arch_ok": arch_ok, "syms_ok": False, "arch": arch}
            continue
        syms_ok = True
        for sym in symbols:
            present = sym in exports
            syms_ok = syms_ok and present
            print(f"      {'OK     ' if present else 'MISSING'} {sym}")
        found[label] = {"present": True, "arch_ok": arch_ok, "syms_ok": syms_ok, "arch": arch}

    # Exports only prove nvapi_QueryInterface exists.  Scan for the ids we
    # actually pass it -- with the package's own x64 image as the control.
    print("\n=== NvAPI entry-point id scan ===")
    id_results = scan_nvapi_entry_point_ids(path)
    private_ids = [n for n, _v, p in NVAPI_ENTRY_POINT_IDS if p]
    arm_counts = id_results.get("arm64") or {}
    x64_counts = id_results.get("x64") or {}
    arm_private_ok = bool(arm_counts) and all(arm_counts.get(n) for n in private_ids)
    x64_private_ok = bool(x64_counts) and all(x64_counts.get(n) for n in private_ids)
    if arm_private_ok:
        id_verdict = ("PRESENT — every private VF id we resolve appears in the arm64 "
                      "image, so the OC surface is built into this driver.")
    elif arm_counts and x64_private_ok:
        id_verdict = ("STRIPPED ON ARM64 — the private VF ids are in the x64 image of "
                      "this same package but not the arm64 one. No VF-curve control.")
    elif arm_counts:
        id_verdict = ("NOT FOUND — the private VF ids appear in neither image, so the "
                      "dispatch table is likely encoded rather than absent. Inconclusive.")
    else:
        id_verdict = "NOT SCANNED — no arm64 NvAPI image was read."
    print("\nEntry-point ids:", id_verdict)

    nvapi = found.get("NVAPI (nvapia64.dll)", {})
    nvml = found.get("NVML (nvml.dll)", {})
    nvapi_ok = nvapi.get("present") and nvapi.get("arch_ok") and nvapi.get("syms_ok")
    nvml_ok = nvml.get("present") and nvml.get("arch_ok") and nvml.get("syms_ok")
    wrong_arch = [lbl for lbl, info in found.items()
                  if info.get("present") and not info.get("arch_ok")]

    if wrong_arch:
        verdict = ("WRONG ARCHITECTURE — " + ", ".join(wrong_arch) +
                   " present but not arm64.  This looks like an x64 driver tree or "
                   "the x64 cross-compilation package, not the Windows-on-Arm one.")
    elif not nvml.get("present") and not nvapi.get("present"):
        verdict = ("NEITHER NVML NOR NVAPI — Green Curve cannot drive a GPU on this "
                   "driver (not a usable target).")
    elif not nvml_ok:
        verdict = "NVML MISSING/INCOMPLETE — no telemetry, power, fan or locked-clock control."
    elif nvapi_ok and arm_private_ok:
        verdict = ("FULL — NVML + NVAPI present as arm64 images AND every private "
                   "pstates20 id we resolve is in the arm64 dispatch table.  VF-curve "
                   "OC/UV should work; only whether the driver HONOURS a write still "
                   "needs real hardware.")
    elif nvapi_ok:
        verdict = ("NVAPI PRESENT, OC SURFACE UNCONFIRMED — the arm64 image exports "
                   "nvapi_QueryInterface but the private pstates20 ids did not scan "
                   "clean; see the id table above.")
    else:
        verdict = ("NVML-ONLY — NVAPI absent: clock offsets / power / fan / locked "
                   "clocks are expected to work, but no VF-curve editing.")
    print("\nVerdict:", verdict)
    return 0 if (nvml_ok or nvapi_ok) else 1

