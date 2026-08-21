// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Before gui_mutation_worker.cpp: the queue drives the in-flight banner's
// animation at both of its transitions, so the banner has to be defined first.
#include "gui_apply_in_flight.cpp"
#include "gui_mutation_worker.cpp"
#include "gui_window_redraw.cpp"
#include "gui_service_state.cpp"
#include "gui_tray_visibility.cpp"
#include "gui_selected_gpu_pnp.cpp"
#include "ui_pending_changes.cpp"

// ============================================================================
// UAC Elevation
// ============================================================================

static HBRUSH g_hBtnBr = nullptr;
static HBRUSH g_hInputBr = nullptr;
static HBRUSH g_hStaticBr = nullptr;
static HBRUSH g_hListBr = nullptr;
static HBRUSH g_hEditBr = nullptr;

static bool is_elevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;

    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    bool result = false;
    if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &size)) {
        result = elev.TokenIsElevated != 0;
    }
    CloseHandle(hToken);
    return result;
}

static void apply_system_titlebar_theme(HWND hwnd) {
    if (!hwnd) return;
    HMODULE d = load_system_library_a("dwmapi.dll");
    if (!d) return;
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = (DwmSetWindowAttribute_t)GetProcAddress(d, "DwmSetWindowAttribute");
    if (setAttr) {
        DWORD lightValue = 1;
        DWORD type = 0, size = sizeof(lightValue);
        LONG useDark = 0;
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, (DWORD*)&type, (LPBYTE)&lightValue, &size) == ERROR_SUCCESS) {
                useDark = (lightValue == 0) ? 1 : 0;
            }
            RegCloseKey(hKey);
        }
        setAttr(hwnd, 20, &useDark, sizeof(useDark));
        setAttr(hwnd, 19, &useDark, sizeof(useDark));
    }
    FreeLibrary(d);
}

// ============================================================================
// GDI Graph Drawing
// ============================================================================

static int g_backbufferWidth = 0;
static int g_backbufferHeight = 0;
static gc_u64 g_guiGdiGeneration = 1;
static gc_u64 g_backbufferGeneration = 0;

static void create_backbuffer(HWND hwnd) {
    destroy_backbuffer();
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right < 1) rc.right = 1;
    if (rc.bottom < 1) rc.bottom = 1;

    // A compatible bitmap may remain backed by a retired display-driver
    // surface after an adapter disable/enable.  Keep the retained backbuffer
    // in process-owned system memory instead; the final BitBlt is the only
    // display-dependent operation and is checked on every paint.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = rc.right;
    info.bmiHeader.biHeight = -rc.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    g_app.hMemBmp = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        &pixels, nullptr, 0);
    if (!g_app.hMemBmp) {
        debug_log_on_change("GUI GDI: CreateDIBSection failed (error %lu)\n",
            GetLastError());
        return;
    }
    g_app.hMemDC = CreateCompatibleDC(nullptr);
    if (!g_app.hMemDC) {
        DeleteObject(g_app.hMemBmp);
        g_app.hMemBmp = nullptr;
        debug_log_on_change("GUI GDI: CreateCompatibleDC failed (error %lu)\n",
            GetLastError());
        return;
    }
    g_app.hOldBmp = (HBITMAP)SelectObject(g_app.hMemDC, g_app.hMemBmp);
    if (!g_app.hOldBmp || g_app.hOldBmp == (HBITMAP)HGDI_ERROR) {
        DeleteDC(g_app.hMemDC);
        DeleteObject(g_app.hMemBmp);
        g_app.hMemDC = nullptr;
        g_app.hMemBmp = nullptr;
        g_app.hOldBmp = nullptr;
        debug_log_on_change("GUI GDI: could not select DIB backbuffer (error %lu)\n",
            GetLastError());
        return;
    }
    g_backbufferWidth = rc.right;
    g_backbufferHeight = rc.bottom;
    g_backbufferGeneration = g_guiGdiGeneration;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(g_app.hMemDC, &rc, bg);
    DeleteObject(bg);
    debug_log("GUI GDI: created system-memory backbuffer %dx%d generation=%llu\n",
        g_backbufferWidth, g_backbufferHeight,
        (unsigned long long)g_guiGdiGeneration);
}

static void fill_window_background(HWND hwnd, HDC hdc) {
    if (!hdc) return;
    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) return;
    HBRUSH brush = g_app.hWindowClassBrush ? g_app.hWindowClassBrush : (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &rc, brush);
}

static void destroy_backbuffer() {
    if (g_app.hMemDC) {
        SelectObject(g_app.hMemDC, g_app.hOldBmp);
        if (g_app.hMemBmp) DeleteObject(g_app.hMemBmp);
        DeleteDC(g_app.hMemDC);
        g_app.hMemDC = nullptr;
        g_app.hMemBmp = nullptr;
        g_app.hOldBmp = nullptr;
    }
    g_backbufferWidth = 0;
    g_backbufferHeight = 0;
    g_backbufferGeneration = 0;
}

static void reset_gui_gdi_generation(const char* reason) {
    ++g_guiGdiGeneration;
    destroy_backbuffer();
    debug_log("GUI GDI: retired display generation; next paint rebuilds DIB generation=%llu (%s)\n",
        (unsigned long long)g_guiGdiGeneration,
        reason && reason[0] ? reason : "display transition");
    if (g_app.hMainWnd && IsWindowVisible(g_app.hMainWnd))
        RedrawWindow(g_app.hMainWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN);
}

#include "ui_main_graph.cpp"

static void draw_gui_scene(HDC hdc, RECT* rc) {
    if (!hdc || !rc) return;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, rc, bg ? bg : (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (bg) DeleteObject(bg);

    if (gui_service_model_ready(&g_app.guiServiceModel))
        draw_graph(hdc, rc);
    else
        gui_draw_service_overlay(hdc, rc);
    // Over the graph, and after the phase overlay: a write can still be in
    // flight while the model briefly leaves READY, and "the write is running"
    // is the more urgent of the two things to say.
    gui_draw_apply_in_flight_banner(hdc, rc);

    HPEN separator = CreatePen(PS_SOLID, 1, COL_GRID);
    if (!separator) return;
    HPEN previous = (HPEN)SelectObject(hdc, separator);
    if (previous && previous != (HPEN)HGDI_ERROR) {
        int graphH = main_layout_graph_height();
        int scrollX = main_layout_scroll_x();
        int scrollY = main_layout_scroll_y();
        MoveToEx(hdc, -scrollX, graphH - scrollY, nullptr);
        LineTo(hdc, main_layout_content_width() - scrollX,
            graphH - scrollY);
        SelectObject(hdc, previous);
    }
    DeleteObject(separator);
}

// ============================================================================
// Edit Controls
// ============================================================================

static void populate_gpu_selector() {
    if (!g_app.hGpuSelectCombo) return;
    begin_programmatic_edit_update();
    SendMessageA(g_app.hGpuSelectCombo, CB_RESETCONTENT, 0, 0);
    unsigned int count = g_app.adapterCount;
    if (count == 0 && g_app.gpuName[0]) count = 1;
    for (unsigned int i = 0; i < count; i++) {
        char label[256] = {};
        if (i < g_app.adapterCount && g_app.adapters[i].valid) {
            format_gpu_adapter_label(&g_app.adapters[i], label, sizeof(label));
            if (g_app.adapters[i].vfBestGuess) {
                StringCchCatA(label, ARRAY_COUNT(label), " - best-effort VF write");
            }
        } else {
            StringCchPrintfA(label, ARRAY_COUNT(label), "0: %s", g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        }
        SendMessageA(g_app.hGpuSelectCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    unsigned int selected = g_app.selectedGpuIndex;
    if (selected >= count) selected = 0;
    SendMessageA(g_app.hGpuSelectCombo, CB_SETCURSEL,
        g_app.configuredGpuSelectionUnresolved ? (WPARAM)-1 : selected, 0);
    EnableWindow(g_app.hGpuSelectCombo,
        ((count > 1 || g_app.configuredGpuSelectionUnresolved) &&
         gui_service_model_ready(&g_app.guiServiceModel)) ? TRUE : FALSE);
    end_programmatic_edit_update();
}

static void apply_gpu_selection_from_ui() {
    if (!g_app.hGpuSelectCombo || programmatic_edit_update_active()) return;
    LRESULT selection = SendMessageA(g_app.hGpuSelectCombo, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection >= MAX_GPU_ADAPTERS) return;
    unsigned int newIndex = (unsigned int)selection;
    if (newIndex == g_app.selectedGpuIndex && g_app.selectedGpuExplicit &&
        !g_app.configuredGpuSelectionUnresolved) return;
    char persistErr[256] = {};
    if (!save_configured_gpu_selection_atomic(newIndex,
            persistErr, sizeof(persistErr))) {
        debug_log("gpu selection: persistence failed for index=%u: %s\n",
            newIndex, persistErr[0] ? persistErr : "unknown error");
        gc_message_box(g_app.hMainWnd,
            persistErr[0] ? persistErr : "Failed to save the selected GPU.",
            "Green Curve", MB_OK | MB_ICONERROR);
        populate_gpu_selector();
        return;
    }
    g_app.selectedGpuIndex = newIndex;
    g_app.selectedNvmlIndex = newIndex;
    g_app.selectedGpuExplicit = true;
    g_app.configuredGpuSelectionUnresolved = false;
    gui_mutation_advance_gpu_epoch("GPU selector");
    reset_gpu_runtime_selection();
    gui_service_begin_full_sync("GPU selection changed");
    set_profile_status_text("Selected GPU %u. Synchronizing live state...", newIndex);
    debug_log("gpu selection changed through GUI: index=%u; asynchronous full sync queued\n",
        newIndex);
}

static void set_edit_value(HWND hEdit, unsigned int value) {
    char buf[16];
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", value);
    begin_programmatic_edit_update();
    SetWindowTextA(hEdit, buf);
    end_programmatic_edit_update();
}

// The VF MHz fields show the same pending values the graph draws, not the
// applied/live readback F-PENDING marks as changed.  With a pending global GPU
// offset an unowned point displays stock + the pending offset component (and
// returns to live readback when the offset is reverted); the locked region
// displays the resolved flat target.  Owned points are rewritten too when the
// offset is SELECTIVE: the curve batch re-places them exactly like unowned
// ones, so leaving their old applied text on screen would disagree with the
// dashed graph.  Under a uniform offset an owned point remains the absolute
// draft the user is typing, and a programmatic SetWindowTextA would steal the
// caret, so that carve-out stays.
static void sync_vf_curve_field_values() {
    if (g_app.numVisible <= 0) return;
    begin_programmatic_edit_update();
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        if (g_app.lockedVi >= 0 && vi >= g_app.lockedVi) {
            // The whole locked region shows the resolved flat target.  The
            // flatten anchor is editable, so it is skipped while it still
            // tracks its anchor and the draft is mid-typing, and once the user
            // retypes it (absolute) its field IS the draft.
            if (vi == g_app.lockedVi && g_app.lockMode != LOCK_MODE_HARD &&
                (!g_app.guiLockTracksAnchor ||
                 !g_app.guiDraft.curveValueValid[ci]))
                continue;
            unsigned int lockTargetMHz = gui_pending_graph_lock_target_mhz();
            if (lockTargetMHz == 0) continue;
            set_edit_value(g_app.hEditsMhz[vi], lockTargetMHz);
            continue;
        }
        bool ownedPoint = g_app.guiCurvePointExplicit[ci];
        // A uniform offset leaves an owned point's field alone; a selective
        // offset projects it like the graph.  Even then, never rewrite the
        // field the user is actively typing in.
        if (ownedPoint &&
            !gui_pending_offset_mode_is_selective(&g_guiPendingChanges))
            continue;
        if (ownedPoint && g_app.hEditsMhz[vi] &&
            GetFocus() == g_app.hEditsMhz[vi])
            continue;
        unsigned int pendingMHz = pending_curve_mhz_for_gui_point(ci);
        if (pendingMHz > 0) set_edit_value(g_app.hEditsMhz[vi], pendingMHz);
    }
    end_programmatic_edit_update();
}

static void populate_edits() {
    bool preserveDirty = gui_state_dirty();
    populate_global_controls();
    GuiServiceActionability actionable = gui_service_actionability_from_app();
    bool serviceReady = gui_service_capability_enabled(
        &actionable, GUI_SERVICE_CAP_EDITOR);
    begin_programmatic_edit_update();
    memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        // Owned points are shown from the drift-free applied-intent baseline, never
        // from live NVAPI readback. The VF curve legitimately drifts under boost/
        // temperature; that drift is telemetry only and must not surface as a
        // configured value in the editor, the graph (via guiCurvePointExplicit +
        // displayed_curve_mhz_for_gui_point), or a subsequent save. Stock/unowned
        // points still show live readback.
        unsigned int ownedMHz = (ci >= 0 && ci < VF_NUM_POINTS) ? g_app.appliedCurveMHz[ci] : 0;
        if (ownedMHz > 0) {
            g_app.guiCurvePointExplicit[ci] = true;
            set_edit_value(g_app.hEditsMhz[vi], ownedMHz);
        } else {
            set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        }
        set_edit_value(g_app.hEditsMv[vi], g_app.curve[ci].volt_uV / 1000);
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], serviceReady ? TRUE : FALSE);
        SendMessageA(g_app.hEditsMv[vi], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMv[vi], FALSE);
        EnableWindow(g_app.hLocks[vi], serviceReady ? TRUE : FALSE);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    }
    // Re-apply lock state if active: disable the tail boxes (and the HARD
    // anchor).  The trailing sync_vf_curve_field_values() fills the locked
    // region with the resolved flat target and projects any pending GPU offset
    // into the unowned fields.
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        // draw_lock_checkbox() derives the tick from g_app.lockedVi/lockMode,
        // so the repaint request is the whole update.
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, FALSE);
        set_edit_value(g_app.hEditsMhz[g_app.lockedVi], g_app.lockedFreq);
        for (int j = g_app.lockedVi + 1; j < g_app.numVisible; j++) {
            // Show live readback value but visually disable to indicate locked state
            SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
            EnableWindow(g_app.hEditsMhz[j], FALSE);
            EnableWindow(g_app.hLocks[j], FALSE);
            InvalidateRect(g_app.hLocks[j], nullptr, FALSE);
        }
        if (!serviceReady) {
            EnableWindow(g_app.hEditsMhz[g_app.lockedVi], FALSE);
            EnableWindow(g_app.hLocks[g_app.lockedVi], FALSE);
        }
    }
    end_programmatic_edit_update();
    if (!preserveDirty && gui_state_dirty()) {
        debug_log("populate_edits: restoring clean GUI state after programmatic repaint\n");
        set_gui_state_dirty(false);
        populate_global_controls();
    }
    gui_pending_changes_refresh();
    // Always after the refresh (not only when it changed): populate_edits has
    // just written applied/live values into every field, and the pending model
    // may be identical while those values must still project.
    sync_vf_curve_field_values();
}

static void apply_lock(int vi, LockMode mode) {
    // Uncheck and re-enable previous lock
    if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
        EnableWindow(g_app.hLocks[g_app.lockedVi], TRUE);
        InvalidateRect(g_app.hLocks[g_app.lockedVi], nullptr, FALSE);
    }

    // Check this one.  The tick follows g_app.lockedVi/lockMode, so the model
    // update below IS the state change; the InvalidateRect after the tail loop
    // setup is what paints it.
    g_app.lockedVi = vi;
    g_app.lockedCi = g_app.visibleMap[vi];
    g_app.lockMode = mode;
    if (g_app.lockedFreq == 0) {
        int ci = g_app.visibleMap[vi];
        if (ci >= 0 && ci < VF_NUM_POINTS &&
            g_app.guiDraft.curveValueValid[ci])
            g_app.lockedFreq = g_app.guiDraft.curveMHz[ci];
    }
    g_app.guiLockTracksAnchor = true;
    if (!programmatic_edit_update_active()) {
        set_gui_state_dirty(true);
        record_ui_action("lock point %d @ %u MHz (%s)", g_app.lockedCi, g_app.lockedFreq, lock_mode_name(mode));
    }
    EnableWindow(g_app.hLocks[vi], TRUE);
    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);

    // Disable tail edit boxes and lock checkboxes to indicate locked state.
    // The pending refresh below fills the disabled boxes with the resolved flat
    // target (anchor plus any GPU-offset delta the lock tracks).
    for (int j = vi + 1; j < g_app.numVisible; j++) {
        SendMessageA(g_app.hEditsMhz[j], EM_SETREADONLY, TRUE, 0);
        EnableWindow(g_app.hEditsMhz[j], FALSE);
        EnableWindow(g_app.hLocks[j], FALSE);
        InvalidateRect(g_app.hLocks[j], nullptr, FALSE);
    }
    gui_pending_changes_refresh();
}

static void sync_locked_tail_preview_from_anchor() {
    if (g_app.lockedVi < 0 || g_app.lockedVi >= g_app.numVisible) return;

    int ci = g_app.visibleMap[g_app.lockedVi];
    if (ci < 0 || ci >= VF_NUM_POINTS ||
        !g_app.guiDraft.curveValueValid[ci]) return;
    int lockMhz = (int)g_app.guiDraft.curveMHz[ci];

    g_app.lockedFreq = (unsigned int)lockMhz;
    if (g_app.lockedCi >= 0) g_app.guiCurvePointExplicit[g_app.lockedCi] = true;
    g_app.guiLockTracksAnchor = false;
    set_gui_state_dirty(true);
    if (g_app.lockedCi >= 0) record_ui_action("lock anchor point %d edited to %u MHz (absolute)", g_app.lockedCi, g_app.lockedFreq);
    // The pending refresh below now fills the disabled tail boxes with the
    // resolved lock target, so an anchor retype updates the previewed tail.
    gui_pending_changes_refresh();
}

static void unlock_all() {
    // Note: NVML locked clocks reset is handled by the service apply pipeline,
    // not here — the GUI process does not own the NVML device handle.

    begin_programmatic_edit_update();
    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.lockMode = LOCK_MODE_NONE;
    g_app.guiLockTracksAnchor = true;
    memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
    set_gui_state_dirty(false);

    for (int vi = 0; vi < g_app.numVisible; vi++) {
        SendMessageA(g_app.hEditsMhz[vi], EM_SETREADONLY, FALSE, 0);
        EnableWindow(g_app.hEditsMhz[vi], TRUE);
        int ci = g_app.visibleMap[vi];
        set_edit_value(g_app.hEditsMhz[vi], displayed_curve_mhz(g_app.curve[ci].freq_kHz));
        EnableWindow(g_app.hLocks[vi], TRUE);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    }
    end_programmatic_edit_update();
    gui_pending_changes_refresh();
}

#include "ui_lock_checkbox.cpp"
#include "ui_oc_hints.cpp"

// Create (or recreate) the hover tooltip that explains the tri-state lock
// checkboxes. The checkboxes are destroyed/recreated on service-state changes,
// so the tooltip and its registered tools are rebuilt alongside them. comctl32
// is loaded dynamically (the GUI does not link it), matching the TaskDialog path.
static void create_lock_tooltips(HWND hParent) {
    if (g_app.hLockTooltip) {
        DestroyWindow(g_app.hLockTooltip);
        g_app.hLockTooltip = nullptr;
    }
    HMODULE comctl = load_system_library_a("comctl32.dll");
    if (!comctl) return;
    static bool s_commonControlsInit = false;
    if (!s_commonControlsInit) {
        typedef BOOL (WINAPI *InitCommonControlsExFn)(const INITCOMMONCONTROLSEX*);
        auto initEx = (InitCommonControlsExFn)GetProcAddress(comctl, "InitCommonControlsEx");
        if (initEx) {
            INITCOMMONCONTROLSEX icc = {};
            icc.dwSize = sizeof(icc);
            icc.dwICC = ICC_BAR_CLASSES;  // registers tooltips_class32
            initEx(&icc);
        }
        s_commonControlsInit = true;
    }
    HWND tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, nullptr, hParent ? (HINSTANCE)GetWindowLongPtrA(hParent, GWLP_HINSTANCE) : g_app.hInst, nullptr);
    if (!tip) return;
    SendMessageA(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)dp(320));
    SendMessageA(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, (LPARAM)MAKELONG(15000, 0));
    apply_tooltip_theme(tip);
    static const char* kLockTip =
        "Lock this point's GPU clock.\r\n"
        "Left-click cycles: off -> flatten -> pin.\r\n"
        "Right-click to choose the mode directly.\r\n"
        "Flatten (check) caps the tail; Pin (dot) hard-locks via NVML.";
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (!g_app.hLocks[vi]) continue;
        TOOLINFOA ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hParent;
        ti.uId = (UINT_PTR)g_app.hLocks[vi];
        ti.lpszText = (LPSTR)kLockTip;
        SendMessageA(tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
    // The three overclock edits are recreated in the same pass as the lock
    // checkboxes, so their range tooltips are registered on the same window and
    // share its lifetime.
    register_oc_range_tooltips(tip, hParent);
    g_app.hLockTooltip = tip;
}

#include "ui_main_controls.cpp"

#include "ui_main_window.cpp"
#include "gui_process_cleanup.cpp"
