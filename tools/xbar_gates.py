"""Source gates for the Blackwell XBAR ClkDomains V2 surface.

Split out of build.py to keep the build script under its size ratchet.  The
dependency is one-way: this module never imports build.py.
"""
import os


def _p(ctx, name):
    return os.path.join(ctx.SOURCE_DIR, name)


def check_xbar_clk_domains(ctx, require_text, forbid_text):
    """F-XBAR-V2 pins the Windows interface and forbids the old ID confusion.

    PropRels controls the GPC-to-XBAR propagation ratio through a different
    structure.  The first implementation used Linux RM command IDs and stored a
    PropRels response as an XBAR snapshot, so neither offset could be read or
    written on Windows.
    """
    backend_h = _p(ctx, "gpu_backend_xbar.h")
    probe_h = _p(ctx, "gpu_capability_probe.cpp")
    apply_cpp = _p(ctx, "gpu_backend_apply.cpp")
    capture_cpp = _p(ctx, "main_runtime_control.cpp")

    require_text(backend_h,
                 "XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL 0xF58938F5u",
                 "F-XBAR-V2: ClkDomains GET ID is pinned")
    require_text(backend_h,
                 "XBAR_NVAPI_CLK_DOMAINS_SET_CONTROL 0xD14B69CFu",
                 "F-XBAR-V2: ClkDomains SET ID is pinned")
    require_text(backend_h,
                 "XBAR_NVAPI_CLK_DOMAINS_VERSION     0x000261A4u",
                 "F-XBAR-V2: ClkDomains structure version/size is pinned")
    require_text(backend_h,
                 "XBAR_NVAPI_CLK_MEASURE             0x527FC458u",
                 "F-XBAR-V2: physical clock measurement ID is pinned")
    require_text(probe_h, "probe_xbar_control_surface(&probe);",
                 "F-XBAR-V2: capability probe uses one focused helper")
    forbid_text(probe_h, "0x20809019u",
                "F-XBAR-V2: Linux RM GET_INFO must not be scanned on Windows")
    forbid_text(probe_h, "0xCBFF71D0u",
                "F-XBAR-V2: PropRels must not be mistaken for ClkDomains")
    forbid_text(apply_cpp, "PROPRELS_GET_CONTROL_ID",
                "F-XBAR-V2: XBAR writes use ClkDomains, not PropRels")
    require_text(capture_cpp,
                 "xbarFreqChanged || xbarVoltChanged) {",
                 "F-XBAR-V2: one-sided edits emit both fields to avoid sibling reset")
