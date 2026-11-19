# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Source gates for VF status point masks, the CMP mining-board warning, the
Windows service single-owner GPU retarget guard and the client/service GPU
selection agreement.

Lives here rather than in build.py so the build script stays under its size
ratchet; build.py passes its own require_text/forbid_text in, and this module
never imports build.py.
"""

import os


def check_source_gates(ctx, require_text, forbid_text, forbid_text_in_operation):
    src = ctx.SOURCE_DIR
    # Every VF STATUS reader sends only the points the 128-point table can hold
    # (vf_point_mask_policy.h).  The full advertised mask makes a driver that
    # cannot serve a requested point refuse the whole read.
    for name in ("gpu_backend.cpp", "linux_backend_vf_read.h", "linux_gpu.cpp",
                 "hardware_capture_getters.h", "main_self_test_vf_mask.cpp"):
        require_text(os.path.join(src, name), "vf_status_request_mask(",
                     f"{name} bounds its VF status request to the point table")
    forbid_text(os.path.join(src, "gpu_backend.cpp"),
                "buf.write_at(backend->statusMaskOffset, mask, sizeof(mask))",
                "the Windows reader never sends the unbounded driver mask to status")
    forbid_text(os.path.join(src, "linux_backend_vf_read.h"),
                "memcpy(buf + b->statusMaskOffset, g->vfMask",
                "the Linux reader never sends the unbounded driver mask to status")
    require_text(os.path.join(src, "main_vf_info_cache.cpp"), "pastTable=",
                 "the VF-info cache logs how far the mask reaches past the table")

    # CMP mining boards: an abortable startup tier with its own opt-out.
    warning = os.path.join(src, "main_capability_warning.cpp")
    require_text(warning, "if (!show_cmp_board_warning(parent)) return false;",
                 "the CMP warning is reached through the single startup call site")
    require_text(warning, "\"hide_cmp_board_warning\"",
                 "the CMP warning has its own persistent opt-out")
    require_text(warning, "TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON",
                 "the CMP warning can be continued once or exited")
    require_text(warning, "pszVerificationText = L\"Do not show this warning again for CMP cards\"",
                 "the CMP warning can be skipped for good")
    require_text(os.path.join(src, "gpu_cmp_policy.h"), "0x1F0Bu",
                 "the confirmed CMP 40HX device id stays recognized")

    # The GUI never resolves a VF backend (it reads the service snapshot), so a
    # warning tier keyed on g_app.vfBackend can never fire there.
    front = os.path.join(src, "main_gpu_front.cpp")
    for anchor, policy in (
            ("static bool should_show_best_guess_warning()",
             "gpu_warning_unrecognized_family_applies(&g_app.selectedGpu)"),
            ("static bool should_show_limited_control_warning()",
             "gpu_warning_limited_control_applies(&g_app.selectedGpu, &g_app.gpuCapability)")):
        forbid_text_in_operation(front, anchor, "g_app.vfBackend",
                                 f"{anchor} judges the snapshot, not the GUI's unset VF backend")
        require_text(front, policy, f"{anchor} uses the snapshot warning policy")
    # Schema-gated advanced clock domains are refused on every board without a
    # pinned layout; only board-level domains make a reduced surface.
    require_text(os.path.join(src, "vf_backends.cpp"),
                 "return gpu_capability_missing_board_domains(probe) != 0;",
                 "the reduced-surface trigger ignores the advanced clock domains")
    require_text(warning, "gc_u32 missing = gpu_capability_missing_board_domains(probe);",
                 "the reduced-surface text lists only board-level domains")

    # Single-owner GPU retarget guard (gpu_intent_owner_policy.h, executed by
    # tests/gpu_intent_owner_tests.cpp). Every service path that can move the
    # one GPU runtime asks the owner policy before it resets the selection,
    # so an active intent cannot be orphaned or lent to another GPU.
    request_policy = os.path.join(src, "main_service_request_policy.cpp")
    ctx.require_order_in_operation(
        request_policy, "static bool service_prepare_requested_gpu(",
        "service_intent_owner_permits_target(&live, change, releasesIntent,",
        "reset_gpu_runtime_selection();",
        "Apply/Reset asks the intent owner before retargeting the GPU runtime")
    lifecycle = os.path.join(src, "main_service_lifecycle_apply.cpp")
    ctx.require_order_in_operation(
        lifecycle, "static bool service_lifecycle_select_context_gpu(",
        "service_intent_owner_permits_target(&target, change, false,",
        "reset_gpu_runtime_selection();",
        "logon asks the intent owner before retargeting the GPU runtime")
    require_text(lifecycle, "} else if (ownerRefused) {",
                 "an owner refusal finishes the logon work instead of retrying it")
    pipe_switch = os.path.join(src, "main_service_pipe_switch.cpp")
    require_text(pipe_switch,
                 "bool ok = service_prepare_requested_gpu(request, true, result,",
                 "only the explicit Reset may release an absent owner's intent")
    forbid_text(pipe_switch, "service_prepare_requested_gpu(request, result,",
                "every Apply/Reset states whether it releases the intent")
    require_text(os.path.join(src, "main.cpp"), '#include "gpu_intent_owner_policy.h"',
                 "the unity build uses the tested owner and identity predicates")
    forbid_text(os.path.join(src, "gpu_backend.cpp"),
                "static bool gpu_adapter_has_same_pci_identity(",
                "PCI identity has one tested definition (gpu_adapter_identity.h)")

    # Client/service GPU selection agreement (gpu_selection_sync_policy.h,
    # executed by tests/gpu_selection_sync_tests.cpp). A full state read names
    # the GPU the client wants; the service switches only through the owner
    # guard and the shared-only policy; a selector choice is saved only after
    # the service shows it; config-driven actions refuse an unshown saved GPU.
    snapshot_cpp = os.path.join(src, "main_service_snapshot_request.cpp")
    ctx.require_order_in_operation(
        snapshot_cpp, "static void service_handle_snapshot_request(",
        "service_snapshot_select_requested_gpu(",
        "bool initialized = hardware_initialize(",
        "a targeted snapshot selects its GPU before the refresh it reports")
    ctx.require_order_in_operation(
        snapshot_cpp, "static bool service_snapshot_select_requested_gpu(",
        "get_machine_restrict_policy(&restrictShared)",
        "service_prepare_requested_gpu(request, false, err, errSize)",
        "a restricted caller cannot choose the GPU; others go through the owner guard")
    require_text(pipe_switch,
                 "service_handle_snapshot_request(request, caller->isAdmin, response);",
                 "the snapshot handler sees the request target and caller tier")
    require_text(os.path.join(src, "gui_mutation_worker.cpp"),
                 "if (kind == GUI_SERVICE_IO_FULL_SYNC && target && target->valid)",
                 "the GUI full state read carries the requested GPU")
    ui_main = os.path.join(src, "ui_main.cpp")
    forbid_text_in_operation(ui_main, "static void apply_gpu_selection_from_ui()",
                             "save_configured_gpu_selection_atomic(",
                             "a selector choice is not saved before the service shows it")
    ctx.require_order_in_operation(
        os.path.join(src, "gui_gpu_selection_sync.cpp"),
        "static void gui_gpu_selection_finish_choice(",
        "gpu_selection_sync_accepted(&completion->requestedTarget, shown,",
        "save_configured_gpu_selection_atomic(",
        "a selector choice is saved only once the service shows that GPU")
    ctx.require_order_in_operation(
        os.path.join(src, "gpu_selection_config.cpp"),
        "static bool validate_configured_gpu_selection_for_client(",
        "gpu_selection_saved_matches_shown(",
        "g_app.selectedGpuIndex = target.nvapiIndex;",
        "config-driven actions refuse a saved GPU that is not shown")
    require_text(os.path.join(src, "entry.cpp"),
                 "if (!validate_configured_gpu_selection_for_cli(",
                 "the CLI asks the service to show the saved GPU before acting")
    ctx.require_order_in_operation(
        os.path.join(src, "main_startup_profiles.cpp"),
        "static void maybe_load_app_launch_profile_to_gui(",
        "if (gui_gpu_selection_switch_expected()) {",
        "validate_configured_gpu_selection_for_client(",
        "the app-start profile waits for the saved GPU instead of refusing early")
