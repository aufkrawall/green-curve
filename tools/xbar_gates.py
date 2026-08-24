"""Source gates for the version-checked XBAR ClkDomains surface.

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
    apply_capture_cpp = _p(ctx, "main_runtime_capture.cpp")
    dialog_cpp = _p(ctx, "xbar_dialog.cpp")
    protocol_header = _p(ctx, "service_protocol.h")
    record_io_h = _p(ctx, "config_profile_xbar_io.h")

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
    require_text(backend_h,
                 "XBAR_PINNED_ENTRY_BASE = 0x124",
                 "F-XBAR-V2-SCHEMA: the validated ClkDomains entry base is pinned")
    require_text(backend_h,
                 "XBAR_PINNED_ENTRY_STRIDE = 0x304",
                 "F-XBAR-V2-SCHEMA: the validated ClkDomains entry stride is pinned")
    require_text(backend_h,
                 "XBAR_PINNED_DOMAIN_COUNT = 8",
                 "F-XBAR-V2-SCHEMA: every validated domain marker is required")
    # Generation independence (unwinder guidance): the .domains structure is
    # versioned per generation and the driver reports which schema it answered
    # with.  Selection must key off that reported version word through a pinned
    # schema table — never off a GPU family whitelist or a fixed-offset read.
    require_text(backend_h, "g_xbarSchemas[] = {",
                 "F-XBAR-SCHEMA-TABLE: schemas live in a version-keyed table")
    require_text(backend_h, "xbar_schema_for_version_word(",
                 "F-XBAR-SCHEMA-TABLE: dispatch keys on the reported version")
    # F-XBAR-SYS: the second aux clock entry ships behind the same validated
    # schema and family-blind probe as XBAR itself.
    require_text(backend_h, "XBAR_PINNED_SYS_ENTRY_INDEX = 3",
                 "F-XBAR-SYS: the empirically identified SYS entry is pinned")
    require_text(backend_h, "xbar_write_entry_freq(",
                 "F-XBAR-SYS: SYS writes reuse the audited single-entry transaction")
    require_text(protocol_header, "SERVICE_MUTATION_DOMAIN_SYS_CLK = 1u << 8",
                 "F-XBAR-SYS: the SYS domain has its own mutation bit")
    require_text(record_io_h, "sys_clk_offset_khz",
                 "F-XBAR-SYS: portable profiles carry the SYS clock key")
    require_text(capture_cpp, "g_app.sysClkProbeValid) {",
                 "F-XBAR-SYS: capture owns SYS only when its surface answered")
    require_text(backend_h, "unvalidated ClkDomains response version",
                 "F-XBAR-DIAGNOSTICS: unknown schema versions log decoded words")
    require_text(backend_h, "snap->valid = false;",
                 "F-XBAR-STALE-PROOF: failed reads invalidate stale snapshots")
    forbid_text(backend_h, "+ XBAR_FREQ_OFFSET_FIELD",
                "F-XBAR-SCHEMA-TABLE: field offsets come from the snapshot,"\
                " not recomputed geometry")
    forbid_text(backend_h, "+ XBAR_MSVDD_OFFSET_FIELD",
                "F-XBAR-SCHEMA-TABLE: field offsets come from the snapshot,"\
                " not recomputed geometry")
    forbid_text(backend_h, "GPU_FAMILY_BLACKWELL",
                "F-XBAR-FAMILY-BLIND: availability is version-decided, not family-gated")
    forbid_text(backend_h, "xbar_discover_entry_layout(",
                "F-XBAR-V2-SCHEMA: ambiguous repeated-pattern discovery must not return")
    require_text(probe_h, "probe_xbar_control_surface(&probe);",
                 "F-XBAR-V2: capability probe uses one focused helper")
    forbid_text(probe_h, "GPU_FAMILY_BLACKWELL",
                "F-XBAR-FAMILY-BLIND: the probe must attempt every adapter")
    forbid_text(probe_h, "0x20809019u",
                "F-XBAR-V2: Linux RM GET_INFO must not be scanned on Windows")
    forbid_text(probe_h, "0xCBFF71D0u",
                "F-XBAR-V2: PropRels must not be mistaken for ClkDomains")
    forbid_text(apply_cpp, "PROPRELS_GET_CONTROL_ID",
                "F-XBAR-V2: XBAR writes use ClkDomains, not PropRels")
    require_text(capture_cpp,
                 "xbarFreqChanged || xbarVoltChanged) {",
                 "F-XBAR-V2: one-sided edits emit both fields to avoid sibling reset")
    # The old Advanced dialog passed its desired client height as the outer
    # CreateWindowExA height, so the caption/border consumed the bottom row.
    require_text(dialog_cpp,
                 "SIZE outerSize = adjusted_window_size_for_client(",
                 "F-XBAR-DIALOG: client size is converted to an outer window frame")
    # F-XBAR-LIVE: the Advanced dialog polls live values every second - the
    # service refreshes the ClkDomains scalars in its 1 Hz telemetry path and
    # the dialog renders them from the envelope without touching edit fields.
    fan_worker_cpp = _p(ctx, "main_service_fan_worker.cpp")
    require_text(fan_worker_cpp, "xbar_refresh_live_state()",
                 "F-XBAR-LIVE: the 1 Hz telemetry path refreshes ClkDomains scalars")
    require_text(dialog_cpp, "XBAR_LIVE_TIMER_ID",
                 "F-XBAR-LIVE: the dialog owns a 1 s live-value timer")
    require_text(dialog_cpp, "xbar_dialog_update_live_values",
                 "F-XBAR-LIVE: live values render through one focused helper")
    require_text(dialog_cpp, "(int)outerSize.cx, (int)outerSize.cy,",
                 "F-XBAR-DIALOG: CreateWindow receives the adjusted frame size")
    forbid_text(dialog_cpp, "x, y, dlgW, dlgH,",
                "F-XBAR-DIALOG: client dimensions are never passed as outer bounds")
    # The pending model enabled Apply for XBAR, but the synchronous capture's
    # "no changes" comparison omitted the domain entirely and rejected it.
    require_text(apply_capture_cpp,
                 "bool xbarUnchanged =",
                 "F-XBAR-V2: manual apply compares captured XBAR intent with hardware")
    require_text(apply_capture_cpp,
                 "powerUnchanged && xbarUnchanged &&",
                 "F-XBAR-V2: no-change and fan-only shortcuts cannot omit XBAR")
    require_text(apply_capture_cpp, "sysClkUnchanged",
                 "F-XBAR-SYS: the no-change/fan-only shortcuts must include SYS")


def check_xbar_profile_contract(ctx, require_text, forbid_text):
    """F-XBAR-PROFILE: saved intent and active ownership cover both fields."""
    record_io_h = _p(ctx, "config_profile_xbar_io.h")
    profiles_cpp = _p(ctx, "config_profiles.cpp")
    gui_state_cpp = _p(ctx, "config_profiles_gui_state.cpp")
    live_cpp = _p(ctx, "main_gpu_state.cpp")
    sync_cpp = _p(ctx, "main_state_sync.cpp")
    helpers_cpp = _p(ctx, "desired_settings_helpers.cpp")
    lifecycle_h = _p(ctx, "service_lifecycle_policy.h")
    windows_merge_cpp = _p(ctx, "config_profiles_ui.cpp")
    helpers_cpp = _p(ctx, "desired_settings_helpers.cpp")
    linux_profiles_cpp = _p(ctx, "linux_port_profiles.cpp")
    telemetry_h = _p(ctx, "xbar_telemetry.h")

    require_text(record_io_h,
                 "load_profile_xbar_settings(",
                 "profile XBAR parsing is centralized and range-checked")
    require_text(profiles_cpp,
                 "load_profile_xbar_settings(path, controlsSection, slot, desired,",
                 "slot profiles use the shared XBAR record parser")
    require_text(profiles_cpp,
                 "resolve_profile_xbar_save_values(desired, &saveControl, haveSaveControl,",
                 "sparse desired records save the effective XBAR baseline")
    forbid_text(profiles_cpp,
                "desired->hasXbarOffsetKhz ? desired->xbarOffsetKhz : 0",
                "profile saves must not fabricate XBAR stock ownership")
    require_text(gui_state_cpp,
                 "int projectedXbarFreqKhz = desired->hasXbarOffsetKhz",
                 "loaded profiles make omitted XBAR fields explicit stock intent")
    require_text(live_cpp,
                 "desired->hasXbarOffsetKhz = g_app.xbarProbeValid &&",
                 "live-state profile saves preserve effective XBAR values")

    require_text(helpers_cpp,
                 "xbar clock ownership differs profile=%d active=%d",
                 "profile identity compares XBAR clock ownership")
    require_text(helpers_cpp,
                 "xbar MSVDD differs profile=%d active=%d",
                 "profile identity compares XBAR MSVDD values")
    require_text(lifecycle_h,
                 "previousIntent->hasXbarOffsetKhz && !nextIntent->hasXbarOffsetKhz",
                 "named-profile transitions clean up omitted owned XBAR fields")
    require_text(windows_merge_cpp,
                 "base->hasXbarOffsetKhz = true;",
                 "Windows sparse merging carries XBAR clock ownership")
    require_text(linux_profiles_cpp,
                 'value = get_section_value(doc, controlsSection, "xbar_offset_khz");',
                 "Linux reads portable XBAR clock profile fields")
    require_text(linux_profiles_cpp,
                 'addControl("xbar_msvdd_offset_uv", value);',
                 "Linux writes portable XBAR MSVDD profile fields")

    require_text(sync_cpp,
                 "snapshot->sysClkSupported = g_app.sysClkProbeValid;",
                 "service snapshots carry truthful SYS clock provenance")
    require_text(sync_cpp,
                 "snapshot->sysClkOffsetReadbackValid = g_app.sysClkFreqReadbackValid;",
                 "service snapshots carry truthful SYS readback provenance")
    require_text(windows_merge_cpp,
                 "base->hasSysClkOffsetKhz = true;",
                 "Windows sparse merging carries SYS clock ownership")
    require_text(gui_state_cpp,
                 "projectedSysClkKhz",
                 "loaded profiles make omitted SYS fields explicit stock intent")
    require_text(helpers_cpp,
                 "sys clock ownership differs profile=%d active=%d",
                 "profile identity compares SYS clock ownership")
    require_text(lifecycle_h,
                 "previousIntent->hasSysClkOffsetKhz && !nextIntent->hasSysClkOffsetKhz",
                 "named-profile transitions clean up omitted owned SYS fields")

    require_text(telemetry_h,
                 "g_app.xbarFreqReadbackValid = false;",
                 "failed XBAR telemetry does not retain stale clock proof")
    require_text(telemetry_h,
                 "g_app.xbarMsvddReadbackValid = false;",
                 "failed XBAR telemetry does not retain stale voltage proof")
    require_text(_p(ctx, "main_runtime_control.cpp"),
                 "if (g_app.xbarProbeValid) {",
                 "capture owns XBAR only when the selected surface answered")
    require_text(_p(ctx, "main_service_apply_runtime.cpp"),
                 "ignored portable XBAR fields; surface unavailable",
                 "portable profiles do not make an unsupported GPU fail")
    require_text(sync_cpp,
                 "snapshot->xbarOffsetReadbackValid = g_app.xbarFreqReadbackValid;",
                 "service snapshots carry truthful XBAR clock provenance")
    require_text(sync_cpp,
                 "snapshot->xbarMsvddOffsetReadbackValid = "
                 "g_app.xbarMsvddReadbackValid;",
                 "service snapshots carry truthful XBAR readback provenance")



def check_video_domain(ctx, require_text, forbid_text):
    """F-XBAR-VIDEO: video = ClkDomains entry 4, identified by differential
    dump against mVolt+ (+400 MHz appeared at 0xE48 = entry 4 + 0x114 and
    HWiNFO confirmed the physical clock).  The engine has no CLK_MEASURE id,
    so exact readback is the verification.  Profiles never own it; any
    profile selection clears it."""
    backend_h = _p(ctx, "gpu_backend_xbar.h")
    apply_cpp = _p(ctx, "gpu_backend_apply.cpp")
    protocol_header = _p(ctx, "service_protocol.h")
    capture_cpp = _p(ctx, "main_runtime_control.cpp")
    lifecycle_h = _p(ctx, "service_lifecycle_policy.h")
    require_text(backend_h, "XBAR_PINNED_VIDEO_ENTRY_INDEX = 4",
                 "F-XBAR-VIDEO: the differentially identified entry is pinned")
    require_text(protocol_header, "SERVICE_MUTATION_DOMAIN_VIDEO_CLK = 1u << 9",
                 "F-XBAR-VIDEO: the video knob has its own mutation bit")
    require_text(capture_cpp, "g_app.videoClkProbeValid) {",
                 "F-XBAR-VIDEO: capture owns VIDEO only when its surface answered")
    require_text(lifecycle_h,
                 "previousIntent->hasVideoClkOffsetKhz && !nextIntent->hasVideoClkOffsetKhz",
                 "F-XBAR-VIDEO: profile selection clears the experiment")


def check_all(ctx, require_text, forbid_text):
    check_xbar_clk_domains(ctx, require_text, forbid_text)
    check_xbar_profile_contract(ctx, require_text, forbid_text)
    check_video_domain(ctx, require_text, forbid_text)
