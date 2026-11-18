// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// GPU support warnings shown once at GUI startup.  Split out of
// main_gpu_state.cpp (which is at its size ratchet) when the second,
// control-surface tier was added for integrated Grace/Blackwell parts.
//
// Included by main_shell.cpp after main_gpu_state.cpp defines the
// unrecognized-family dialog and before entry.cpp calls into this file.
// The CMP mining-board tier was added here too (gpu_cmp_policy.h).

#include "gpu_cmp_policy.h"

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
            "Show this warning again on next launch?\n"
            "(Select No to disable this warning)",
            g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
    } else {
        StringCchPrintfA(message, ARRAY_COUNT(message),
            "%s reports a reduced control surface (%s).\n\n"
            "The driver refused or does not expose: %s.\n\n"
            "This is expected on integrated Grace/Blackwell parts, where system memory "
            "is shared with the CPU, the power budget belongs to the whole SoC, and the "
            "fan is usually owned by the platform rather than the NVIDIA driver. It is "
            "also common on notebook boards, where the laptop vendor keeps the power "
            "target (TGP) and often the fan under its own firmware.\n\n"
            "Everything else still works, and Green Curve will attempt the domains that "
            "did answer.\n\n"
            "Show this warning again on next launch?\n"
            "(Select No to disable this warning)",
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
                    L"NVIDIA driver. It is also common on notebook boards, where the laptop "
                    L"vendor keeps the power target (TGP) and often the fan under its own "
                    L"firmware.\n\n"
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

// Third tier: an NVIDIA CMP mining board (gpu_cmp_policy.h).  The family is a
// known one, so nothing about the VF layout is guessed; what is untested is
// the modified driver such a board needs.  Abortable like the family warning:
// OK continues this time, the checkbox (or No in the fallback box) continues
// and stops asking, Cancel exits.  Its own config key, so silencing another
// tier never silences this one.
static bool s_cmpBoardWarningShownThisSession = false;

static bool should_show_cmp_board_warning(bool* matchedByIdOut) {
    const bool byId = gpu_cmp_device_id_confirmed(g_app.selectedGpu.pciInfoValid != 0,
                                                  g_app.selectedGpu.deviceId);
    const bool byName = gpu_cmp_name_matches(g_app.gpuName) ||
                        gpu_cmp_name_matches(g_app.selectedGpu.name);
    if (matchedByIdOut) *matchedByIdOut = byId;
    if (!byId && !byName) return false;
    if (get_config_int(g_app.configPath, "warnings", "hide_cmp_board_warning", 0) != 0) {
        debug_log_on_change("CMP board warning suppressed by user config\n");
        return false;
    }
    return !s_cmpBoardWarningShownThisSession;
}

static bool show_cmp_board_warning(HWND parent) {
    bool matchedById = false;
    if (!should_show_cmp_board_warning(&matchedById)) return true;
    const char* gpuName = g_app.gpuName[0] ? g_app.gpuName
        : (g_app.selectedGpu.name[0] ? g_app.selectedGpu.name : "NVIDIA GPU");
    debug_log("CMP board warning: gpu=%s device=0x%08X pciValid=%d matchedBy=%s\n",
              gpuName, g_app.selectedGpu.deviceId, g_app.selectedGpu.pciInfoValid ? 1 : 0,
              matchedById ? "device id" : "name");

    static const char kBody[] =
        "%s is an NVIDIA CMP mining card. Green Curve can only tune it through a "
        "modified NVIDIA driver, and its support is community-reported on a CMP 40HX "
        "only.\n\n"
        "Clocks, voltages, power and fan control may not behave like on a GeForce "
        "card, and a profile that is stable on one driver build may not be on "
        "another.\n\n"
        "Check applied clocks and stability carefully after changes.";
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
            config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            config.pszWindowTitle = L"Green Curve - CMP Mining Card";
            config.pszMainIcon = TD_WARNING_ICON;
            config.pszMainInstruction = L"Experimental support for an NVIDIA CMP mining card";
            config.pszVerificationText = L"Do not show this warning again for CMP cards";
            char body[1024] = {};
            StringCchPrintfA(body, ARRAY_COUNT(body), kBody, gpuName);
            WCHAR content[1024] = {};
            utf8_to_wide(body, content, ARRAY_COUNT(content));
            config.pszContent = content;

            int button = 0;
            BOOL verificationChecked = FALSE;
            if (SUCCEEDED(taskDialogIndirect(&config, &button, nullptr, &verificationChecked))) {
                handled = true;
                if (button == IDCANCEL) {
                    debug_log("CMP board warning: user chose to exit\n");
                    FreeLibrary(comctl);
                    remove_tray_icon();
                    release_single_instance_mutex();
                    return false;
                }
                suppressWarning = verificationChecked != FALSE;
            }
        }
        FreeLibrary(comctl);
    }

    if (!handled) {
        char message[1280] = {};
        StringCchPrintfA(message, ARRAY_COUNT(message), kBody, gpuName);
        StringCchCatA(message, ARRAY_COUNT(message),
            "\n\nContinue with this GPU?\n- Yes: Continue this time\n"
            "- No: Continue and do not show this warning again\n- Cancel: Exit");
        int result = gc_message_box(parent, message, "Green Curve - CMP Mining Card",
                                    MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL || result == 0) {
            debug_log("CMP board warning: user chose to exit\n");
            remove_tray_icon();
            release_single_instance_mutex();
            return false;
        }
        suppressWarning = result == IDNO;
    }

    if (suppressWarning) {
        if (set_config_int(g_app.configPath, "warnings", "hide_cmp_board_warning", 1)) {
            debug_log("CMP board warning disabled by user\n");
        } else {
            debug_log("CMP board warning: failed to persist suppression flag\n");
        }
    } else {
        debug_log("CMP board warning: continued for this session\n");
    }
    s_cmpBoardWarningShownThisSession = true;
    return true;
}

// Single startup entry point for every tier, so the GUI has one call site and
// the ordering (abortable warnings first, the informational surface warning
// last) lives here rather than in entry.cpp.  Returns false only when the user
// chose to exit at an abortable warning.
static bool show_gpu_support_warnings(HWND parent) {
    if (!show_best_guess_support_warning(parent)) return false;
    if (!show_cmp_board_warning(parent)) return false;
    // No-op unless the read-only probe actually found a domain missing, so
    // validated discrete GPUs never see this one.
    show_limited_control_surface_warning(parent);
    return true;
}
