// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Hover tooltips advertising the supported overclock ranges on the GPU offset,
// memory offset, and power limit fields.  The bounds themselves are derived by
// the pure policy in oc_range_hint_policy.h; this file only owns the Win32
// surface and the change-gated refresh.
//
// Note on placeholders: the edit-control cue banner cannot be used here (and a
// build gate forbids reintroducing it).  Windows paints a cue banner only while
// the control is empty, and all three fields always hold a value ("0"/"0"/"100"
// at creation, then repopulated from every service snapshot by
// populate_global_controls).  In-field hint text would therefore never be
// visible, which is why the ranges live in hover tooltips.

#include "oc_range_hint_policy.h"

static void refresh_oc_range_hints();

struct OcRangeHintCache {
    bool valid;
    OcRangeInputs inputs;
};

static OcRangeHintCache s_ocRangeHintCache = {};
static HWND s_ocRangeTooltip = nullptr;

// Drop every binding to controls that are about to be (or have just been)
// recreated.  The tooltip window does not survive a control rebuild, so a
// cached "nothing changed" verdict from the previous generation would leave the
// fresh tooltips empty and the handle dangling.
static void oc_range_hints_invalidate() {
    s_ocRangeHintCache.valid = false;
    s_ocRangeTooltip = nullptr;
}

static OcRangeInputs oc_range_inputs_from_app() {
    OcRangeInputs in = {};
    in.gpuKnown = g_app.gpuOffsetRangeKnown;
    in.gpuMinMHz = g_app.gpuClockOffsetMinMHz;
    in.gpuMaxMHz = g_app.gpuClockOffsetMaxMHz;
    in.memKnown = g_app.memOffsetRangeKnown;
    in.memMinMHz = g_app.memClockOffsetMinMHz;
    in.memMaxMHz = g_app.memClockOffsetMaxMHz;
    in.powerMinmW = g_app.powerLimitMinmW;
    in.powerMaxmW = g_app.powerLimitMaxmW;
    in.powerDefaultmW = g_app.powerLimitDefaultmW;
    return in;
}

static bool oc_range_inputs_equal(const OcRangeInputs* a, const OcRangeInputs* b) {
    if (!a || !b) return false;
    return a->gpuKnown == b->gpuKnown && a->gpuMinMHz == b->gpuMinMHz &&
           a->gpuMaxMHz == b->gpuMaxMHz && a->memKnown == b->memKnown &&
           a->memMinMHz == b->memMinMHz && a->memMaxMHz == b->memMaxMHz &&
           a->powerMinmW == b->powerMinmW && a->powerMaxmW == b->powerMaxmW &&
           a->powerDefaultmW == b->powerDefaultmW;
}

static void oc_range_update_tooltip_text(HWND edit, const char* text) {
    if (!s_ocRangeTooltip || !edit || !text) return;
    TOOLINFOA ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = GetParent(edit);
    ti.uId = (UINT_PTR)edit;
    ti.lpszText = (LPSTR)text;
    SendMessageA(s_ocRangeTooltip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
}

// Paint the shared tooltip window in the application palette instead of the
// system's info-tip yellow, which looked like a foreign element next to the
// dark main window.  TTM_SETTIPBKCOLOR / TTM_SETTIPTEXTCOLOR are honored only
// while the control has no visual styles applied; the app ships a manifest
// without a comctl32 v6 dependency, so that already holds and these take
// effect.  SetWindowTheme(L"", L"") makes the requirement explicit and keeps
// this correct if a v6 dependency is ever added.
static void apply_tooltip_theme(HWND tip) {
    if (!tip) return;
    typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = load_system_library_a("uxtheme.dll");
    if (ux) {
        auto setWindowTheme = (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme");
        if (setWindowTheme) setWindowTheme(tip, L"", L"");
    }
    SendMessageA(tip, TTM_SETTIPBKCOLOR, (WPARAM)COL_TOOLTIP_BG, 0);
    SendMessageA(tip, TTM_SETTIPTEXTCOLOR, (WPARAM)COL_TOOLTIP_TEXT, 0);
    SendMessageA(tip, WM_SETFONT, (WPARAM)get_ui_font(), TRUE);
    debug_log("tooltip theme applied: hwnd=%p bk=%06lX text=%06lX\n",
              (void*)tip, (unsigned long)COL_TOOLTIP_BG,
              (unsigned long)COL_TOOLTIP_TEXT);
}

// Register the three overclock edits with the tooltip window that
// create_lock_tooltips() just built.  The edits are destroyed and recreated
// alongside the lock checkboxes on every control rebuild, so registration
// happens in the same pass and the text is pushed immediately afterwards
// rather than waiting for the next service snapshot.
static void register_oc_range_tooltips(HWND tip, HWND hParent) {
    oc_range_hints_invalidate();
    if (!tip) return;
    s_ocRangeTooltip = tip;
    HWND edits[3] = { g_app.hGpuOffsetEdit, g_app.hMemOffsetEdit,
                      g_app.hPowerLimitEdit };
    for (int i = 0; i < 3; i++) {
        if (!edits[i]) continue;
        TOOLINFOA ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hParent;
        ti.uId = (UINT_PTR)edits[i];
        ti.lpszText = (LPSTR)"";
        if (!SendMessageA(tip, TTM_ADDTOOLA, 0, (LPARAM)&ti)) {
            debug_log("OC range tooltip registration FAILED: index=%d hwnd=%p\n",
                      i, (void*)edits[i]);
        }
    }
    refresh_oc_range_hints();
}

// Recompute the advertised ranges and push them into the tooltips.  Called from
// populate_global_controls(), which runs on every service snapshot, so the
// writes are gated on an actual change in the driver-reported bounds.
static void refresh_oc_range_hints() {
    OcRangeInputs in = oc_range_inputs_from_app();
    if (s_ocRangeHintCache.valid &&
        oc_range_inputs_equal(&s_ocRangeHintCache.inputs, &in)) return;
    s_ocRangeHintCache.valid = true;
    s_ocRangeHintCache.inputs = in;

    OcRangeBounds gpu = oc_range_gpu_offset(&in);
    OcRangeBounds mem = oc_range_mem_offset(&in);
    OcRangeBounds power = oc_range_power_pct(&in);
    debug_log("OC range hints refreshed: gpu known=%d %d..%d mem known=%d %d..%d "
              "power %d..%d %% (from %d/%d/%d mW) tooltip=%p\n",
        gpu.known ? 1 : 0, gpu.min, gpu.max,
        mem.known ? 1 : 0, mem.min, mem.max,
        power.min, power.max,
        in.powerMinmW, in.powerMaxmW, in.powerDefaultmW,
        (void*)s_ocRangeTooltip);

    char tip[512] = {};
    oc_range_format_gpu_tip(tip, sizeof(tip), &in);
    oc_range_update_tooltip_text(g_app.hGpuOffsetEdit, tip);
    oc_range_format_mem_tip(tip, sizeof(tip), &in);
    oc_range_update_tooltip_text(g_app.hMemOffsetEdit, tip);
    oc_range_format_power_tip(tip, sizeof(tip), &in);
    oc_range_update_tooltip_text(g_app.hPowerLimitEdit, tip);
}
