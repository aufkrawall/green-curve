// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// F-INFLIGHT, main-window half: the banner drawn while a hardware write is
// running, and the repaint timer that animates it.
//
// The first version of this feature only set the background-service status line
// at the bottom of the window and the tray tooltip.  Both were reported as "I
// am not seeing any status indicator" -- correctly: a line of text at the
// bottom edge is easy to miss, and the tray tooltip only exists while the
// cursor hovers the icon.  An apply takes seconds by design, so the state needs
// to be visible without being looked for.
//
// The banner sits inside the GRAPH, in the same scrolled content coordinates
// draw_graph() uses, starting immediately below MAIN_LAYOUT_GRAPH_TOP_MARGIN.
//
// It was first pinned to the client area at y=0 so it could never be scrolled
// out of view.  That was wrong, and visibly so: every control in this window is
// a CHILD window placed at `y - scrollY`, and a child paints over its parent
// rather than being clipped by it.  The GPU selector lives in exactly that top
// strip, so it appeared to bleed through the banner -- and no client-pinned
// rectangle can avoid that in general, because scrolling moves a different set
// of controls under it.  Content coordinates below the graph's top margin are
// the one region guaranteed to hold no child control at any scroll position.
//
// The geometry of the sweep is pure (gui_apply_in_flight_policy.h) and
// unit-tested; this file only turns it into GDI calls.

static unsigned int g_guiApplyInFlightFrame = 0;
static bool g_guiApplyInFlightTimerActive = false;

// Content-coordinate strip the banner occupies.  Returned even when no write is
// in flight, because the invalidate that CLEARS the banner needs the same rect.
static RECT gui_apply_in_flight_banner_rect() {
    RECT banner = {};
    GuiApplyBannerBand band = gui_apply_in_flight_banner_band(
        main_layout_graph_height(),
        dp(MAIN_LAYOUT_GRAPH_TOP_MARGIN_LOGICAL), dp(70));
    if (!band.visible) return banner;
    banner.left = 0;
    banner.top = band.top;
    banner.right = main_layout_content_width();
    banner.bottom = band.bottom;
    return banner;
}

static void gui_draw_apply_in_flight_banner(HDC hdc, const RECT* client) {
    (void)client;
    if (!hdc || !g_app.applyInFlight) return;
    RECT banner = gui_apply_in_flight_banner_rect();
    if (banner.right <= banner.left || banner.bottom <= banner.top) return;
    // Same transform draw_graph() paints under, so the banner tracks the graph
    // it overlays instead of sliding across it as the window scrolls.
    int savedDc = SaveDC(hdc);
    SetViewportOrgEx(hdc, -main_layout_scroll_x(), -main_layout_scroll_y(),
        nullptr);

    HBRUSH panel = CreateSolidBrush(COL_INFLIGHT_PANEL);
    if (panel) {
        FillRect(hdc, &banner, panel);
        DeleteObject(panel);
    }
    HBRUSH border = CreateSolidBrush(COL_INFLIGHT_BORDER);
    if (border) {
        FrameRect(hdc, &banner, border);
        DeleteObject(border);
    }

    int inset = dp(14);
    char headline[128] = {};
    char detail[192] = {};
    gui_apply_in_flight_status_text(headline, ARRAY_COUNT(headline));
    gui_apply_in_flight_detail_text(detail, ARRAY_COUNT(detail));
    SetBkMode(hdc, TRANSPARENT);
    HFONT previous = (HFONT)SelectObject(hdc,
        g_app.hCachedFont ? g_app.hCachedFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT label = banner;
    label.left += inset;
    label.right -= inset;
    label.top += dp(9);
    SetTextColor(hdc, COL_TEXT);
    DrawTextA(hdc, headline, -1, &label,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    label.top += dp(19);
    SetTextColor(hdc, COL_LABEL);
    DrawTextA(hdc, detail, -1, &label,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, previous);

    // The indeterminate sweep, along the bottom edge of the banner.  Single
    // exit from here on: the viewport transform above must be restored on every
    // path, including the degenerate ones.
    RECT track = banner;
    track.left += inset;
    track.right -= inset;
    track.bottom -= dp(8);
    track.top = track.bottom - dp(4);
    if (track.right > track.left && track.bottom > track.top) {
        HBRUSH trackBrush = CreateSolidBrush(COL_INFLIGHT_TRACK);
        if (trackBrush) {
            FillRect(hdc, &track, trackBrush);
            DeleteObject(trackBrush);
        }
        GuiApplySweep sweep = gui_apply_in_flight_sweep(
            track.right - track.left, g_guiApplyInFlightFrame);
        RECT bar = track;
        bar.left = track.left + sweep.x;
        bar.right = bar.left + sweep.width;
        // Clip to the track by hand: the bar deliberately starts and ends
        // outside it, so it enters and leaves instead of popping at the edges.
        if (bar.left < track.left) bar.left = track.left;
        if (bar.right > track.right) bar.right = track.right;
        if (sweep.width > 0 && bar.right > bar.left) {
            HBRUSH sweepBrush = CreateSolidBrush(COL_INFLIGHT_SWEEP);
            if (sweepBrush) {
                FillRect(hdc, &bar, sweepBrush);
                DeleteObject(sweepBrush);
            }
        }
    }
    RestoreDC(hdc, savedDc);
}

static void gui_apply_in_flight_invalidate_banner() {
    if (!g_app.hMainWnd) return;
    RECT banner = gui_apply_in_flight_banner_rect();
    if (banner.right <= banner.left || banner.bottom <= banner.top) return;
    // Content coordinates -> client coordinates, the same shift the controls
    // and the graph are placed with.
    OffsetRect(&banner, -main_layout_scroll_x(), -main_layout_scroll_y());
    // Only the banner strip: the scene is redrawn into the backbuffer either
    // way, but BeginPaint clips the blit to the update region, so an animation
    // frame does not push a full-window blit ten times a second.
    InvalidateRect(g_app.hMainWnd, &banner, FALSE);
}

// Start/stop the animation with the state it animates.  Called from the two
// mutation-queue transitions, which is the only place that owns the flag.
static void gui_apply_in_flight_set_animation(bool active) {
    if (!g_app.hMainWnd) return;
    if (active == g_guiApplyInFlightTimerActive) return;
    g_guiApplyInFlightTimerActive = active;
    if (active) {
        g_guiApplyInFlightFrame = 0;
        SetTimer(g_app.hMainWnd, APPLY_IN_FLIGHT_TIMER_ID,
            GUI_APPLY_IN_FLIGHT_FRAME_MS, nullptr);
    } else {
        KillTimer(g_app.hMainWnd, APPLY_IN_FLIGHT_TIMER_ID);
    }
    // Paint the arrival or the departure of the banner immediately rather than
    // waiting for the first tick / the next unrelated repaint.
    gui_apply_in_flight_invalidate_banner();
}

static void gui_apply_in_flight_on_timer() {
    if (!g_app.applyInFlight) {
        // Defensive: the flag is authoritative, so a timer that outlived it
        // stops itself instead of animating a banner nobody is drawing.
        gui_apply_in_flight_set_animation(false);
        return;
    }
    g_guiApplyInFlightFrame++;
    gui_apply_in_flight_invalidate_banner();
}
