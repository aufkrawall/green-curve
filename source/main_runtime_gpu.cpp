#include "gpu_backend_curve_write.h"

#include "curve_point_verify_policy.h"

static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz) {
    return curve_display_mhz_from_khz((unsigned int)displayed_curve_khz(rawFreq_kHz));
}

// Delegates to the pure, platform-shared tolerance rule (curve_point_verify_policy.h)
// over g_app.curve, so Windows and Linux cannot drift onto different answers
// for the same quantization.
static unsigned int curve_point_verify_tolerance_mhz(int pointIndex) {
    return curve_point_verify_tolerance_mhz_over(
        [](int ci) { return g_app.curve[ci].freq_kHz; }, pointIndex, VF_NUM_POINTS);
}

static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize) {
    if (!desired) {
        set_message(detail, detailSize, "No requested curve state to verify");
        return false;
    }

    auto matches_target = [](int pointIndex, unsigned int actualMHz, unsigned int targetMHz) -> bool {
        unsigned int toleranceMHz = curve_point_verify_tolerance_mhz(pointIndex);
        int diff = (int)actualMHz - (int)targetMHz;
        return diff >= -(int)toleranceMHz && diff <= (int)toleranceMHz;
    };

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (lockedTailMask && lockedTailMask[ci]) continue;
        if (g_app.curve[ci].freq_kHz == 0) {
            set_message(detail, detailSize, "Requested VF point %d has no readback", ci);
            return false;
        }

        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int targetMHz = desired->curvePointMHz[ci];
        if (!matches_target(ci, actualMHz, targetMHz)) {
            set_curve_target_mismatch_detail(ci, actualMHz, targetMHz, false, detail, detailSize);
            return false;
        }
    }

    if (lockedTailMask && lockMhz > 0) {
        bool sawTailPoint = false;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!lockedTailMask[ci]) continue;
            if (g_app.curve[ci].freq_kHz == 0) {
            set_message(detail, detailSize, "Requested VF point %d has no readback", ci);
            return false;
        }

            sawTailPoint = true;
            unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (!matches_target(ci, actualMHz, lockMhz)) {
                set_curve_target_mismatch_detail(ci, actualMHz, lockMhz, true, detail, detailSize);
                return false;
            }
        }
        if (!sawTailPoint) {
            set_message(detail, detailSize, "No VF points were available to verify the curve lock");
            return false;
        }
    }

    if (detail && detailSize > 0) detail[0] = 0;
    return true;
}

static int raw_curve_khz_from_display_mhz(unsigned int displayMHz) {
    long long v = (long long)displayMHz * 1000LL;
    if (v < 0) v = 0;
    return (int)v;
}

static int curve_base_khz_for_point(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return 0;
    long long base = (long long)g_app.curve[pointIndex].freq_kHz - (long long)g_app.freqOffsets[pointIndex];
    if (base < 0) base = 0;
    return (int)base;
}

// Refresh the cached stock-base estimate from the latest snapshot. Unshaped
// snapshots refresh the topology-bound stock curve cache; shaped snapshots
// correct cross-talked neighbors and monotonically clamped tails so GUI
// previews never stack offsets onto driver cross-talk or clamped tail deltas.
static void update_gui_stock_base_estimate() {
    const bool hardPinned = g_app.appliedLockMode == LOCK_MODE_HARD &&
        g_app.appliedLockFreq > 0;
    const bool captured = vf_stock_curve_cache_observe(
        &g_app.stockCurveCache, g_app.curve, g_app.freqOffsets,
        g_app.vfFloorBinding.floor, g_app.gpuClockOffsetkHz, hardPinned);
    if (captured) {
        debug_log_on_change("gui stock base: captured unshaped stock snapshot topologyKey=0x%08X end=%d\n",
            g_app.stockCurveCache.topologyKey, g_app.stockCurveCache.graphicsEnd);
    }
    g_app.stockBaseEstimate = vf_estimate_stock_base(
        g_app.curve, g_app.freqOffsets, g_app.vfFloorBinding.floor,
        &g_app.stockCurveCache, g_app.gpuClockOffsetkHz);
    if (g_app.stockBaseEstimate.crosstalkCount > 0 ||
        g_app.stockBaseEstimate.clampedTailCount > 0) {
        debug_log_on_change(
            "gui stock base: source=%s thermalDelta=%lld kHz refs=%d crosstalk=%d firstCi=%d raw=%lld->est=%lld kHz clampedTail=%d firstTailCi=%d\n",
            g_app.stockBaseEstimate.usedCachedStock ? "cached-stock" : "shaped-interp",
            g_app.stockBaseEstimate.thermalDeltaKHz,
            g_app.stockBaseEstimate.referenceCount,
            g_app.stockBaseEstimate.crosstalkCount,
            g_app.stockBaseEstimate.firstCrosstalkCi,
            g_app.stockBaseEstimate.firstCrosstalkRawKHz,
            g_app.stockBaseEstimate.firstCrosstalkEstKHz,
            g_app.stockBaseEstimate.clampedTailCount,
            g_app.stockBaseEstimate.firstClampedTailCi);
    }
}

static int curve_stock_base_khz_for_point(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return 0;
    if (g_app.stockBaseEstimate.valid && g_app.stockBaseEstimate.baseKHz[pointIndex] > 0) {
        long long base = g_app.stockBaseEstimate.baseKHz[pointIndex];
        if (base > INT_MAX) base = INT_MAX;
        return (int)base;
    }
    return curve_base_khz_for_point(pointIndex);
}

static int curve_delta_khz_for_target_display_mhz_unclamped(int pointIndex, unsigned int displayMHz) {
    long long target = (long long)raw_curve_khz_from_display_mhz(displayMHz);
    long long base = (long long)curve_base_khz_for_point(pointIndex);
    long long delta = target - base;
    return (int)delta;
}

static int curve_delta_khz_for_target_display_mhz(int pointIndex, unsigned int displayMHz) {
    return clamp_freq_delta_khz(curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, displayMHz));
}

static void set_curve_target_mismatch_detail(int pointIndex, unsigned int actualMHz, unsigned int targetMHz, bool lockTail, char* detail, size_t detailSize) {
    int requiredDeltaKHz = curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, targetMHz);
    int minkHz = 0;
    int maxkHz = 0;
    bool rangeKnown = get_curve_offset_range_khz(&minkHz, &maxkHz);
    unsigned int voltMV = 0;
    if (pointIndex >= 0 && pointIndex < VF_NUM_POINTS) {
        voltMV = g_app.curve[pointIndex].volt_uV / 1000;
    }

    if (rangeKnown && requiredDeltaKHz < minkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the minimum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the minimum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (rangeKnown && requiredDeltaKHz > maxkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the maximum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the maximum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (lockTail) {
        set_message(detail, detailSize,
            "Lock tail verified at %u MHz @ %u mV instead of requested %u MHz",
            actualMHz, voltMV, targetMHz);
    } else {
        set_message(detail, detailSize,
            "VF point %d verified at %u MHz instead of requested %u MHz",
            pointIndex, actualMHz, targetMHz);
    }
}

static int mem_display_mhz_from_driver_khz(int driver_kHz) {
    return driver_kHz / 1000; // actual clock kHz to actual MHz
}

static int mem_driver_khz_from_display_mhz(int displayMHz) {
    return displayMHz * 1000; // actual clock kHz
}

static int mem_display_mhz_from_driver_mhz(int driverMHz) {
    return driverMHz / 2; // NVML memory offset MHz is effective; UI mirrors actual MHz
}

// Coherent-projection paint suppression.  The depth counter lives beside the
// invalidation helpers rather than in gui_window_redraw.cpp (which owns the
// transaction but is GUI-only) because invalidate_main_window() has to honour
// it and is compiled into the service binary too, where the depth never leaves
// zero.  gui_window_redraw.cpp is the only writer.
#include "gui_window_redraw_policy.h"
static_assert(GUI_REDRAW_STYLE_CHILD == (unsigned int)WS_CHILD,
    "the pure redraw policy must describe the real WS_CHILD bit");
static_assert(GUI_REDRAW_STYLE_VISIBLE == (unsigned int)WS_VISIBLE,
    "the pure redraw policy must describe the real WS_VISIBLE bit");
static int g_guiTopLevelRedrawDepth = 0;

static bool gui_top_level_paint_suppressed() {
    return g_guiTopLevelRedrawDepth > 0;
}

static void invalidate_main_window() {
    if (!app_main_window()) return;
    if (gui_window_invalidation_must_defer(gui_top_level_paint_suppressed(),
            g_app.trayWindowHiddenIntent,
            IsWindowVisible(g_app.hMainWnd) != FALSE)) {
        // Background profile/service changes update the resident control model,
        // but a tray-hidden owner has no frame to present.  Leave one deferred
        // invalidation for the next explicit show; an immediate WM_PAINT is
        // both wasted work and a needless opportunity for hidden-window side
        // effects in future paint code.
        //
        // The same deferral covers a running projection transaction, which owns
        // the next frame: it used to be implied by the top-level WM_SETREDRAW
        // pair making the window read as invisible here, and has to be stated
        // now that the transaction no longer touches WS_VISIBLE.
        RedrawWindow(g_app.hMainWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN);
        return;
    }
    redraw_window_sync(g_app.hMainWnd);
}

static void redraw_window_sync(HWND hwnd) {
    if (!hwnd) return;
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

static void flush_desktop_composition() {
    typedef HRESULT (WINAPI *dwm_flush_t)();
    static dwm_flush_t dwmFlush = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE dwm = load_system_library_a("dwmapi.dll");
        if (dwm) dwmFlush = (dwm_flush_t)GetProcAddress(dwm, "DwmFlush");
        resolved = true;
    }
    if (dwmFlush) dwmFlush();
}

static void show_window_with_primed_first_frame(HWND hwnd, int nCmdShow) {
    if (!hwnd) return;

    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    redraw_window_sync(hwnd);
    flush_desktop_composition();

    SetWindowPos(hwnd, nullptr, wr.left, wr.top, winW, winH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, nCmdShow);
    redraw_window_sync(hwnd);
    update_fan_telemetry_timer();
}

static bool is_system_dark_theme_active() {
    DWORD value = 1;
    DWORD type = 0;
    DWORD size = sizeof(value);
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    bool dark = false;
    if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
        dark = value == 0;
    }
    RegCloseKey(hKey);
    return dark;
}

static void initialize_dark_mode_support() {
    if (s_darkModeResolved) return;
    s_darkModeResolved = true;

    HMODULE ux = load_system_library_a("uxtheme.dll");
    if (!ux) return;

    s_fnAllowDarkModeForWindow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    s_fnSetPreferredAppMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    s_fnFlushMenuThemes = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));

    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void refresh_menu_theme_cache() {
    initialize_dark_mode_support();
    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void allow_dark_mode_for_window(HWND hwnd) {
    if (!hwnd) return;
    initialize_dark_mode_support();
    if (s_fnAllowDarkModeForWindow) {
        s_fnAllowDarkModeForWindow(hwnd, is_system_dark_theme_active() ? TRUE : FALSE);
    }
}

