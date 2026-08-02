// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "gui_graph_axis_policy.h"

// VF graph rendering for the GDI main window.  Split out of ui_main.cpp when the
// applied/pending two-series drawing was added and that file passed the
// ~800-line guideline.
//
// The graph is a projection of the accepted model plus GuiDraft; it never
// parses edit-control text.  It draws up to two series:
//
//   applied  solid COL_CURVE   - drift-free applied intent (appliedCurveMHz),
//                                falling back to live readback for points Green
//                                Curve does not own.  This is what the GPU runs.
//   pending  dashed COL_PENDING - what Apply would write, drawn only while
//                                F-PENDING reports a curve or lock change, with
//                                orange markers on exactly the changed points.
//
// With nothing pending the two series are identical and only the applied one is
// drawn, so a clean window looks exactly as it did before the feature.

// The LIVE half of a plotted point: what the driver reports for it right now.
// Reached only for a point the editor does not determine -- the editor half
// (draft value, lock target, offset component) is resolved once by
// gui_pending_resolve_graph_preview() and read back below.
//
// Note: the locked tail override was removed after the uniform floor offset
// fix (Build 109) eliminated tail drift. Real driver-reported values now match
// the lock target, so no intent-vs-reality substitution is needed.
static unsigned int displayed_curve_mhz_for_gui_point(int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return 0;
    return displayed_curve_mhz(g_app.curve[ci].freq_kHz);
}

// What the GPU has applied right now.  0 in appliedCurveMHz means the applied
// intent does not own that point, so live readback is the applied truth there.
static unsigned int applied_curve_mhz_for_gui_point(int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return 0;
    // A hard NVML pin runs the whole GPU at one clock, so every plotted point
    // is the applied lock target, not the (still-shaped) VF readback below the
    // anchor.  The solid curve then reads as the flat line the pin actually is.
    if (g_app.appliedLockMode == LOCK_MODE_HARD &&
        g_app.appliedLockFreq > 0) {
        return g_app.appliedLockFreq;
    }
    if (g_app.appliedCurveMHz[ci]) return g_app.appliedCurveMHz[ci];
    return displayed_curve_mhz(g_app.curve[ci].freq_kHz);
}

// What Apply would write.  The DECISION -- locked tail vs owned point vs a point
// a changed global offset is about to move -- belongs to
// gui_pending_resolve_graph_preview(), which mirrors gpu_backend_apply.cpp and
// is what the repaint gate compares.  Re-deriving it here is what let the graph
// and the gate disagree: an edit could change the plotted value without
// changing anything the gate looked at, and the new curve then waited for an
// unrelated repaint.  All that is left here is materialising the editor's
// resolved intent against live readback.
static unsigned int pending_curve_mhz_for_gui_point(int ci) {
    GuiGraphPreviewPoint preview = gui_pending_graph_preview(ci);
    if (preview.absoluteMHz) return preview.absoluteMHz;
    if (preview.offsetFromStockBase) {
        // Stock + the PENDING offset component. The point currently displays
        // stock + the APPLIED component, so reusing its displayed value would
        // silently miss every point a changed GPU offset is about to move.
        long long targetkHz = (long long)curve_base_khz_for_point(ci)
            + (long long)preview.offsetComponentMHz * 1000LL;
        if (targetkHz < 0) targetkHz = 0;
        if (targetkHz > UINT_MAX) targetkHz = UINT_MAX;
        return displayed_curve_mhz((unsigned int)targetkHz);
    }
    return displayed_curve_mhz_for_gui_point(ci);
}

static void log_gui_locked_tail_display_drift_if_needed() {
    static int lastLockCi = -2;
    static unsigned int lastLockMHz = 0;
    static int lastDriftCount = -1;
    static int lastMaxDriftCi = -1;
    static unsigned int lastMaxDeltaMHz = 0;

    if (g_app.lockedCi < 0 || g_app.lockedCi >= VF_NUM_POINTS || g_app.lockedFreq == 0) {
        lastLockCi = -2;
        lastLockMHz = 0;
        lastDriftCount = -1;
        lastMaxDriftCi = -1;
        lastMaxDeltaMHz = 0;
        return;
    }

    int driftCount = 0;
    int maxDriftCi = -1;
    unsigned int maxDeltaMHz = 0;
    for (int ci = g_app.lockedCi; ci < VF_NUM_POINTS; ci++) {
        if (!is_curve_point_visible_in_gui(ci)) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int liveMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int deltaMHz = liveMHz > g_app.lockedFreq
            ? liveMHz - g_app.lockedFreq
            : g_app.lockedFreq - liveMHz;
        if (deltaMHz <= 2) continue;
        driftCount++;
        if (deltaMHz > maxDeltaMHz) {
            maxDeltaMHz = deltaMHz;
            maxDriftCi = ci;
        }
    }

    if (driftCount > 0
        && (g_app.lockedCi != lastLockCi
            || g_app.lockedFreq != lastLockMHz
            || driftCount != lastDriftCount
            || maxDriftCi != lastMaxDriftCi
            || maxDeltaMHz != lastMaxDeltaMHz)) {
        debug_log("gui locked tail live readback drift: ci=%d lock=%u MHz drifted=%d max=ci%d/%uMHz temp=%d valid=%d\n",
            g_app.lockedCi,
            g_app.lockedFreq,
            driftCount,
            maxDriftCi,
            maxDeltaMHz,
            g_app.gpuTemperatureC,
            g_app.gpuTemperatureValid ? 1 : 0);
    }

    lastLockCi = g_app.lockedCi;
    lastLockMHz = g_app.lockedFreq;
    lastDriftCount = driftCount;
    lastMaxDriftCi = maxDriftCi;
    lastMaxDeltaMHz = maxDeltaMHz;
}

static void draw_graph(HDC hdc, RECT* rc) {
    (void)rc;
    int w = main_layout_content_width();
    int h = main_layout_graph_height();
    int savedDc = SaveDC(hdc);
    SetViewportOrgEx(hdc, -main_layout_scroll_x(), -main_layout_scroll_y(), nullptr);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(COL_PANEL);
    RECT graphRc = {0, 0, w, h};
    FillRect(hdc, &graphRc, bgBrush);
    DeleteObject(bgBrush);

    if (!g_app.backgroundServiceAvailable) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = g_app.backgroundServiceInstalled
            ? "Background service not responding. Live controls disabled."
            : "Background service not installed. Live controls disabled.";
        TextOutA(hdc, w / 2 - dp(170), h / 2 - dp(8), msg, (int)strlen(msg));
        RestoreDC(hdc, savedDc);
        return;
    }

    if (!g_app.loaded) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        const char* msg = "Background service running, waiting for GPU snapshot...";
        TextOutA(hdc, w / 2 - dp(150), h / 2 - dp(8), msg, (int)strlen(msg));
        RestoreDC(hdc, savedDc);
        return;
    }

    // Axis ranges.  Both axes are sized from the plotted data, which is
    // exactly the VISIBLE VF list -- the same points the editor shows -- so
    // high clocks never hit the ceiling, low clocks do not float in empty
    // space, and a GPU whose curve begins at 750 mV does not show an empty
    // 700-750 mV cell on the left.
    const int VOLT_AXIS_MIN_MV = MIN_VISIBLE_VOLT_mV;
    const int VOLT_AXIS_MAX_MV = 1250;
    const int VOLT_AXIS_GRID_STEP_MV = 50;
    const int VOLT_AXIS_MIN_SPAN_MV = 300;
    const int VOLT_AXIS_HEADROOM_MV = 10;
    const int FREQ_AXIS_MIN_MHZ = MIN_VISIBLE_FREQ_MHz;
    const int FREQ_AXIS_GRID_STEP_MHZ = 500;
    const int FREQ_AXIS_MIN_SPAN_MHZ = 1000;
    const int FREQ_AXIS_HEADROOM_MHZ = 200;

    int axisDataMinMHz = INT_MAX;
    int axisDataMaxMHz = 0;
    int axisDataMinMv = INT_MAX;
    int axisDataMaxMv = 0;
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int volt_mv = g_app.curve[ci].volt_uV / 1000;
        unsigned int appliedMHz = applied_curve_mhz_for_gui_point(ci);
        unsigned int pendingMHz = pending_curve_mhz_for_gui_point(ci);
        unsigned int hi = appliedMHz > pendingMHz ? appliedMHz : pendingMHz;
        unsigned int lo = appliedMHz < pendingMHz ? appliedMHz : pendingMHz;
        if (hi == 0) continue;
        if (lo > 0 && (int)lo < axisDataMinMHz) axisDataMinMHz = (int)lo;
        if ((int)hi > axisDataMaxMHz) axisDataMaxMHz = (int)hi;
        if ((int)volt_mv < axisDataMinMv) axisDataMinMv = (int)volt_mv;
        if ((int)volt_mv > axisDataMaxMv) axisDataMaxMv = (int)volt_mv;
    }
    if (axisDataMinMHz > axisDataMaxMHz) axisDataMinMHz = axisDataMaxMHz;
    if (axisDataMinMv > axisDataMaxMv) axisDataMinMv = axisDataMaxMv;
    GuiGraphAxisRange axisRange = gui_graph_frequency_axis(
        axisDataMinMHz == INT_MAX ? 0 : axisDataMinMHz, axisDataMaxMHz,
        FREQ_AXIS_MIN_MHZ, FREQ_AXIS_GRID_STEP_MHZ, FREQ_AXIS_MIN_SPAN_MHZ,
        FREQ_AXIS_HEADROOM_MHZ);
    GuiGraphVoltageRange voltRange = gui_graph_voltage_axis(
        axisDataMinMv == INT_MAX ? 0 : axisDataMinMv, axisDataMaxMv,
        VOLT_AXIS_MIN_MV, VOLT_AXIS_MAX_MV, VOLT_AXIS_GRID_STEP_MV,
        VOLT_AXIS_MIN_SPAN_MV, VOLT_AXIS_HEADROOM_MV);
    const int MIN_FREQ_MHz = axisRange.minMHz;
    const int MAX_FREQ_MHz = axisRange.maxMHz;
    const int MIN_VOLT_mV = voltRange.minMv;
    const int MAX_VOLT_mV = voltRange.maxMv;
    static int lastAxisMinMHz = -1, lastAxisMaxMHz = -1,
        lastVoltMinMv = -1, lastVoltMaxMv = -1;
    if (MIN_FREQ_MHz != lastAxisMinMHz || MAX_FREQ_MHz != lastAxisMaxMHz ||
        MIN_VOLT_mV != lastVoltMinMv || MAX_VOLT_mV != lastVoltMaxMv) {
        lastAxisMinMHz = MIN_FREQ_MHz;
        lastAxisMaxMHz = MAX_FREQ_MHz;
        lastVoltMinMv = MIN_VOLT_mV;
        lastVoltMaxMv = MAX_VOLT_mV;
        debug_log("gui graph frequency axis: %d..%d MHz (data %d..%d) voltage %d..%d mV (data %d..%d)\n",
            MIN_FREQ_MHz, MAX_FREQ_MHz,
            axisDataMinMHz == INT_MAX ? 0 : axisDataMinMHz, axisDataMaxMHz,
            MIN_VOLT_mV, MAX_VOLT_mV,
            axisDataMinMv == INT_MAX ? 0 : axisDataMinMv, axisDataMaxMv);
    }

    // DPI-scaled margins
    int ml = dp(70), mr = dp(30),
        mt = dp(MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL), mb = dp(55);
    int pw = w - ml - mr;
    int ph = h - mt - mb;

    // Helper: map voltage mV to X pixel
    auto volt_to_x = [&](unsigned int mv) -> int {
        if (mv < (unsigned)MIN_VOLT_mV) mv = MIN_VOLT_mV;
        if (mv > (unsigned)MAX_VOLT_mV) mv = MAX_VOLT_mV;
        return ml + (int)((long long)(mv - MIN_VOLT_mV) * pw / (MAX_VOLT_mV - MIN_VOLT_mV));
    };

    // Helper: map frequency MHz to Y pixel
    auto freq_to_y = [&](unsigned int mhz) -> int {
        if (mhz < (unsigned)MIN_FREQ_MHz) mhz = MIN_FREQ_MHz;
        if (mhz > (unsigned)MAX_FREQ_MHz) mhz = MAX_FREQ_MHz;
        return mt + ph - (int)((long long)(mhz - MIN_FREQ_MHz) * ph / (MAX_FREQ_MHz - MIN_FREQ_MHz));
    };

    // GDI objects (cached in AppData to avoid churn across paint cycles)
    HPEN gridPen = g_app.hCachedGridPen ? g_app.hCachedGridPen : CreatePen(PS_SOLID, 1, COL_GRID);
    HPEN axisPen = g_app.hCachedAxisPen ? g_app.hCachedAxisPen : CreatePen(PS_SOLID, 1, COL_AXIS);
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
    HFONT hFont = g_app.hCachedFont ? g_app.hCachedFont : CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hFontSmall = g_app.hCachedFontSmall ? g_app.hCachedFontSmall : CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // Vertical grid lines (voltage axis, every 50mV, label every 100mV)
    for (int mv = MIN_VOLT_mV; mv <= MAX_VOLT_mV; mv += 50) {
            int x = volt_to_x((unsigned int)mv);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, x, mt, nullptr);
        LineTo(hdc, x, mt + ph);

        if (mv % 100 == 0) {
            SelectObject(hdc, hFontSmall);
            SetTextColor(hdc, COL_LABEL);
            char buf[16];
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mv);
            SIZE sz;
            GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
            TextOutA(hdc, x - sz.cx / 2, mt + ph + dp(4), buf, (int)strlen(buf));
        }
    }

    // Horizontal grid lines (frequency axis, every 500MHz, label every 500MHz)
    for (int mhz = MIN_FREQ_MHz; mhz <= MAX_FREQ_MHz; mhz += 500) {
        int y = freq_to_y((unsigned int)mhz);
        SelectObject(hdc, gridPen);
        MoveToEx(hdc, ml, y, nullptr);
        LineTo(hdc, ml + pw, y);

        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, COL_LABEL);
        char buf[16];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", mhz);
        SIZE sz;
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz);
        TextOutA(hdc, ml - sz.cx - dp(6), y - sz.cy / 2, buf, (int)strlen(buf));
    }

    // Axes
    SelectObject(hdc, axisPen);
    MoveToEx(hdc, ml, mt, nullptr);
    LineTo(hdc, ml, mt + ph);
    MoveToEx(hdc, ml, mt + ph, nullptr);
    LineTo(hdc, ml + pw, mt + ph);

    // Axis titles
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    const char* xTitle = "Voltage (mV)";
    SIZE sz;
    GetTextExtentPoint32A(hdc, xTitle, (int)strlen(xTitle), &sz);
    TextOutA(hdc, ml + pw / 2 - sz.cx / 2, mt + ph + dp(24), xTitle, (int)strlen(xTitle));

    // The y-axis title is rotated bottom-to-top (font escapement 900) and
    // centred beside the labels, so it can never overlap the top clock label
    // the way the old horizontal title at the top-left did.
    const char* yTitle = "Frequency (MHz)";
    HFONT hFontRot = CreateFontA(dp(13), 0, 900, 900, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (hFontRot) {
        HFONT oldRotFont = (HFONT)SelectObject(hdc, hFontRot);
        GetTextExtentPoint32A(hdc, yTitle, (int)strlen(yTitle), &sz);
        // With escapement 900 the text runs upward from the origin, so start
        // it below the vertical centre by half its (horizontal) length.
        TextOutA(hdc, dp(10), mt + ph / 2 + sz.cx / 2, yTitle,
            (int)strlen(yTitle));
        SelectObject(hdc, oldRotFont);
        DeleteObject(hFontRot);
    }

    log_gui_locked_tail_display_drift_if_needed();

    auto plot_point = [&](unsigned int mhz, unsigned int mv, POINT* out) -> bool {
        if (mhz == 0 && mv == 0) return false;
        // Only plot points within our visible range
        if (mv < (unsigned)MIN_VOLT_mV || mv > (unsigned)MAX_VOLT_mV) return false;
        if (mhz < (unsigned)MIN_FREQ_MHz || mhz > (unsigned)MAX_FREQ_MHz) return false;
        out->x = volt_to_x(mv);
        out->y = freq_to_y(mhz);
        return true;
    };

    bool showPending =
        gui_pending_domain_changed(GUI_PENDING_CURVE | GUI_PENDING_LOCK);
    bool hardPinPending = g_app.lockMode == LOCK_MODE_HARD &&
        gui_pending_graph_lock_target_mhz() > 0;
    COLORREF appliedColor =
        (g_app.appliedLockMode == LOCK_MODE_HARD &&
         g_app.appliedLockFreq > 0) ? COL_CURVE_PINNED : COL_CURVE;
    COLORREF pendingColor = hardPinPending ? COL_CURVE_PINNED : COL_PENDING;

    // Build both polylines: sorted by voltage, only within our plotted ranges.
    POINT pendingPts[VF_NUM_POINTS];   // the editor's intent; carries the labels
    unsigned int pendingMHzForPt[VF_NUM_POINTS] = {};
    bool pendingChangedForPt[VF_NUM_POINTS] = {};
    POINT appliedPts[VF_NUM_POINTS];
    POINT changedPts[VF_NUM_POINTS];
    int nPending = 0, nApplied = 0, nChanged = 0;
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int i = g_app.visibleMap[vi];
        if (i < 0 || i >= VF_NUM_POINTS) continue;
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        unsigned int pendingMHz = pending_curve_mhz_for_gui_point(i);
        if (plot_point(pendingMHz, volt_mv, &pendingPts[nPending])) {
            pendingMHzForPt[nPending] = pendingMHz;
            if (showPending && (gui_pending_curve_point_is_changed(i) ||
                                hardPinPending)) {
                pendingChangedForPt[nPending] = true;
                changedPts[nChanged++] = pendingPts[nPending];
            }
            nPending++;
        }
        unsigned int appliedMHz = applied_curve_mhz_for_gui_point(i);
        if (plot_point(appliedMHz, volt_mv, &appliedPts[nApplied])) nApplied++;
    }

    if (nApplied > 1) {
        draw_curve_polyline_smooth(hdc, appliedPts, nApplied, dp(2),
                                   appliedColor);
        SelectObject(hdc, oldPen);
    }

    draw_curve_points_ringed(hdc, appliedPts, nApplied, dp(2), dp(4),
                             appliedColor, COL_POINT);
    SelectObject(hdc, oldPen);

    if (showPending) {
        // Only the stretches that actually differ, each expanded by one point so
        // the dashed line departs from and rejoins the applied curve where the
        // two are equal. Drawing the complete pending curve would cover the
        // applied one along its whole length and make a two-point edit look like
        // a change to every point.
        for (GuiPendingRun run =
                 gui_pending_next_changed_run(pendingChangedForPt, nPending, 0);
             run.valid;
             run = gui_pending_next_changed_run(pendingChangedForPt, nPending,
                                                run.nextScan)) {
            int runCount = run.drawLast - run.drawFirst + 1;
            if (runCount > 1)
                draw_curve_polyline_smooth(hdc, pendingPts + run.drawFirst,
                                           runCount, dp(2), pendingColor, true);
        }
        SelectObject(hdc, oldPen);
        if (nChanged > 0) {
            draw_curve_points_ringed(hdc, changedPts, nChanged, dp(2), dp(4),
                                     COL_PENDING, COL_PENDING);
            SelectObject(hdc, oldPen);
        }
    }

    // Frequency labels on the curve the editor holds (every ~10th plotted point).
    // Coloured per point, not per graph: a label only reads as pending when that
    // specific point differs from what the GPU has applied.
    SelectObject(hdc, hFontSmall);
    for (int i = 0; i < nPending; i += nvmax(1, nPending / 10)) {
        SetTextColor(hdc, pendingChangedForPt[i] ? pendingColor : COL_TEXT);
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%u", pendingMHzForPt[i]);
        SIZE sz2;
        GetTextExtentPoint32A(hdc, buf, (int)strlen(buf), &sz2);
        // Skip a label that would spill left of the plot (the first point sits
        // at the axis floor): it used to waste the left margin.
        if (pendingPts[i].x - sz2.cx / 2 < ml + dp(4)) continue;
        TextOutA(hdc, pendingPts[i].x - sz2.cx / 2, pendingPts[i].y - dp(16),
                 buf, (int)strlen(buf));
    }

    // Info line at top: always show live peak from driver readback
    SelectObject(hdc, hFont);
    SetTextColor(hdc, COL_TEXT);
    char info[512];
    {
        unsigned int actualMaxFreq = 0;
        unsigned int actualMaxVolt = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (g_app.curve[i].freq_kHz > actualMaxFreq) {
                actualMaxFreq = g_app.curve[i].freq_kHz;
                actualMaxVolt = g_app.curve[i].volt_uV;
            }
        }
        // The headline lock names the same cached target the tail is drawn at
        // (anchor plus any GPU-offset delta the lock tracks) and that Apply
        // will write, not the possibly stale g_app.lockedFreq.
        unsigned int lockTargetMHz = gui_pending_graph_lock_target_mhz();
        if (g_app.lockedVi >= 0 && g_app.lockedCi >= 0 && lockTargetMHz > 0) {
            unsigned int lockVoltMv = g_app.curve[g_app.lockedCi].volt_uV / 1000;
            StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Lock: %u MHz @ %u mV  |  Live peak: %u MHz @ %u mV",
                             g_app.gpuName, g_app.numPopulated,
                             lockTargetMHz, lockVoltMv,
                             displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
        } else {
            StringCchPrintfA(info, ARRAY_COUNT(info), "%s  |  %d pts  |  Peak: %u MHz @ %u mV",
                             g_app.gpuName, g_app.numPopulated,
                             displayed_curve_mhz(actualMaxFreq), actualMaxVolt / 1000);
        }
        static unsigned int lastPeakMHz = 0, lastPeakMv = 0;
        unsigned int peakMHz = displayed_curve_mhz(actualMaxFreq);
        unsigned int peakMv = actualMaxVolt / 1000;
        if (peakMHz != lastPeakMHz || peakMv != lastPeakMv) {
            lastPeakMHz = peakMHz;
            lastPeakMv = peakMv;
            debug_log("gui live peak: %u MHz @ %u mV\n", peakMHz, peakMv);
        }
    }
    TextOutA(hdc, ml + dp(6), dp(4), info, (int)strlen(info));

    // Cleanup
    SelectObject(hdc, oldFont);
    if (!g_app.hCachedFont) DeleteObject(hFont);
    if (!g_app.hCachedFontSmall) DeleteObject(hFontSmall);
    if (!g_app.hCachedGridPen) DeleteObject(gridPen);
    if (!g_app.hCachedAxisPen) DeleteObject(axisPen);
    RestoreDC(hdc, savedDc);
}
