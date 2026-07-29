// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// GPU support warnings shown once at GUI startup.  Split out of
// main_gpu_state.cpp (which is at its size ratchet) when the second,
// control-surface tier was added for integrated Grace/Blackwell parts.
//
// Included by main_shell.cpp after main_gpu_state.cpp defines the
// unrecognized-family dialog and before entry.cpp calls into this file.

// Second warning tier.  Unlike the unrecognized-family dialog this one is
// purely informational: the surface is known, it is simply smaller than a
// discrete board's, so there is nothing to abort — the user keeps every domain
// that did answer.  No Cancel/exit path, and it returns void for that reason.
static void show_limited_control_surface_warning(HWND parent) {
    if (!should_show_limited_control_warning()) return;

    const GpuCapabilityProbe* probe = &g_app.gpuCapability;
    gc_u32 missing = gpu_capability_missing_domains(probe);

    // Build the "what you lose" list from the probe rather than from any
    // assumption about which part this is.
    char missingList[256] = {};
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        if (!(missing & gpu_capability_mask_for_index(i))) continue;
        if (missingList[0]) StringCchCatA(missingList, ARRAY_COUNT(missingList), ", ");
        StringCchCatA(missingList, ARRAY_COUNT(missingList), gpu_capability_domain_name(i));
    }
    if (!missingList[0]) StringCchCopyA(missingList, ARRAY_COUNT(missingList), "(none)");

    const char* surfaceName = gpu_control_surface_class_name(
        gpu_capability_surface_class(probe));
    // Two different situations reach this dialog, and they need different text.
    // The topology-contradiction case can arrive with NOTHING missing, where
    // "reduced control surface (full) — the driver does not expose: (none)"
    // would be nonsense.
    bool integratedOnly = missing == 0 &&
        gpu_capability_topology_contradicts_discrete(probe);

    debug_log("limited control-surface warning: gpu=%s surface=%s missing=%s"
              " topology=%s integratedOnly=%d\n",
              g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU", surfaceName, missingList,
              gpu_memory_topology_name(probe->memoryTopology), integratedOnly ? 1 : 0);

    char message[768] = {};
    if (integratedOnly) {
        StringCchPrintfA(message, ARRAY_COUNT(message),
            "%s shares one memory pool with the CPU, so it is an integrated part "
            "rather than a discrete graphics card — even though it reports a "
            "familiar GPU architecture.\n\n"
            "Every control Green Curve uses did answer on this system, so it will "
            "work normally. But no hardware of this class has been tested, and a "
            "memory clock change here targets system RAM rather than a separate "
            "VRAM pool.\n\n"
            "Check applied clocks and offsets carefully after changes.\n\n"
            "No disables this warning.",
            g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
    } else {
        StringCchPrintfA(message, ARRAY_COUNT(message),
            "%s reports a reduced control surface (%s).\n\n"
            "The driver refused or does not expose: %s.\n\n"
            "This is expected on integrated Grace/Blackwell parts, where system memory "
            "is shared with the CPU, the power budget belongs to the whole SoC, and the "
            "fan is usually owned by the platform rather than the NVIDIA driver.\n\n"
            "Everything else still works, and Green Curve will attempt the domains that "
            "did answer.\n\n"
            "No disables this warning.",
            g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU", surfaceName, missingList);
    }

    bool handled = false;
    bool suppressWarning = false;
    HMODULE comctl = load_system_library_a("comctl32.dll");
    if (comctl) {
        typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto taskDialogIndirect = (TaskDialogIndirect_t)GetProcAddress(comctl, "TaskDialogIndirect");
        if (taskDialogIndirect) {
            TASKDIALOGCONFIG config = {};
            config.cbSize = sizeof(config);
            config.hwndParent = parent;
            config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            config.dwCommonButtons = TDCBF_OK_BUTTON;
            config.pszWindowTitle = integratedOnly
                ? L"Green Curve - Untested Integrated GPU"
                : L"Green Curve - Reduced Control Surface";
            config.pszMainIcon = TD_INFORMATION_ICON;
            config.pszMainInstruction = integratedOnly
                ? L"This is an integrated GPU, not a discrete card"
                : L"This GPU exposes fewer controls than a discrete board";
            config.pszVerificationText = L"Do not show this warning again for this GPU";

            WCHAR content[2048] = {};
            if (integratedOnly) {
                StringCchPrintfW(content, ARRAY_COUNT(content),
                    L"%hs shares one memory pool with the CPU, so it is an integrated "
                    L"part rather than a discrete graphics card — even though it "
                    L"reports a familiar GPU architecture.\n\n"
                    L"Every control Green Curve uses did answer on this system, so it "
                    L"will work normally. But no hardware of this class has been "
                    L"tested, and a memory clock change here targets system RAM rather "
                    L"than a separate VRAM pool.\n\n"
                    L"Check applied clocks and offsets carefully after changes.",
                    g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
            } else {
                StringCchPrintfW(content, ARRAY_COUNT(content),
                    L"%hs reports a reduced control surface (%hs).\n\n"
                    L"The driver refused or does not expose: %hs.\n\n"
                    L"This is expected on integrated Grace/Blackwell parts, where system "
                    L"memory is shared with the CPU, the power budget belongs to the whole "
                    L"SoC, and the fan is usually owned by the platform rather than the "
                    L"NVIDIA driver.\n\n"
                    L"Everything else still works, and Green Curve will attempt the domains "
                    L"that did answer.",
                    g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU", surfaceName, missingList);
            }
            config.pszContent = content;

            int button = 0;
            BOOL verificationChecked = FALSE;
            HRESULT hr = taskDialogIndirect(&config, &button, nullptr, &verificationChecked);
            if (SUCCEEDED(hr)) {
                handled = true;
                suppressWarning = verificationChecked != FALSE;
            }
        }
        FreeLibrary(comctl);
    }

    if (!handled) {
        int result = gc_message_box(parent, message,
                                    integratedOnly
                                        ? "Green Curve - Untested Integrated GPU"
                                        : "Green Curve - Reduced Control Surface",
                                    MB_YESNO | MB_ICONINFORMATION);
        suppressWarning = result == IDNO;
    }

    if (suppressWarning) {
        if (set_config_int(g_app.configPath, "warnings", "hide_limited_control_warning", 1)) {
            debug_log("limited control-surface warning disabled by user\n");
        } else {
            debug_log("limited control-surface warning: failed to persist suppression flag\n");
        }
    }

    g_limitedControlWarningShownThisSession = true;
}

// Single startup entry point for both tiers, so the GUI has one call site and
// the ordering (abortable family warning first, informational surface warning
// second) lives here rather than in entry.cpp.  Returns false only when the
// user chose to exit at the family warning.
static bool show_gpu_support_warnings(HWND parent) {
    if (!show_best_guess_support_warning(parent)) return false;
    // No-op unless the read-only probe actually found a domain missing, so
    // validated discrete GPUs never see this one.
    show_limited_control_surface_warning(parent);
    return true;
}
