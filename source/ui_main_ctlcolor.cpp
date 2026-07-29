// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// WM_CTLCOLOR* handling for the main window, split out of ui_main_window.cpp so
// that file stays under its size ratchet.
//
// This is also where F-PENDING colours the native controls.  A field whose
// pending value differs from what the GPU has applied draws its text in
// COL_PENDING; a disabled one (the locked tail, a fan control that does not
// apply in the current mode) uses COL_PENDING_DIM so it still reads as
// disabled.  The background never changes, so the existing cached brushes are
// reused and no new GDI lifetime is introduced.
//
// Both handlers matter: Windows sends WM_CTLCOLOREDIT for an ordinary edit but
// WM_CTLCOLORSTATIC for a disabled or ES_READONLY one, so the locked-tail MHz
// edits are coloured by the static path.
//
// The pending state is read from the cached F-PENDING summary, never by
// scraping control text -- the same rule the graph follows.

static COLORREF gui_main_input_text_color(HWND control) {
    bool enabled = control && IsWindowEnabled(control);
    if (gui_pending_edit_is_changed(control))
        return enabled ? COL_PENDING : COL_PENDING_DIM;
    return enabled ? COL_TEXT : COL_LABEL;
}

static HBRUSH gui_main_ctlcolor_btn(HDC hdc) {
    SetBkColor(hdc, COL_BG);
    if (!g_hBtnBr) g_hBtnBr = CreateSolidBrush(COL_BG);
    return g_hBtnBr;
}

static HBRUSH gui_main_ctlcolor_static(HDC hdc, HWND control) {
    char className[16] = {};
    if (control) GetClassNameA(control, className, ARRAY_COUNT(className));
    LONG_PTR style = control ? GetWindowLongPtrA(control, GWL_STYLE) : 0;
    bool isEditInput = strcmp(className, "Edit") == 0 &&
        (((style & ES_READONLY) != 0) || !IsWindowEnabled(control));
    if (control == g_app.hFanModeCombo || control == g_app.hProfileCombo ||
        control == g_app.hAppLaunchCombo || control == g_app.hLogonCombo ||
        isEditInput) {
        SetTextColor(hdc, gui_main_input_text_color(control));
        SetBkColor(hdc, COL_INPUT);
        if (!g_hInputBr) g_hInputBr = CreateSolidBrush(COL_INPUT);
        return g_hInputBr;
    }
    // The two checkbox captions that used to be STATICs here are drawn by the
    // owner-draw checkbox renderer now (F-CHECKBOX-HIT), which is also what
    // gives them a themed greyed state instead of the system grey-text colour.
    if (control == g_app.hServiceStatusLabel ||
        control == g_app.hLogonHintLabel) {
        COLORREF textColor = (control == g_app.hServiceStatusLabel &&
            !g_app.backgroundServiceInstalled &&
            !g_app.backgroundServiceToggleInFlight)
            ? RGB(0xFF, 0x60, 0x60) : COL_TEXT;
        SetTextColor(hdc, textColor);
    } else {
        SetTextColor(hdc, COL_LABEL);
    }
    SetBkColor(hdc, COL_BG);
    if (!g_hStaticBr) g_hStaticBr = CreateSolidBrush(COL_BG);
    return g_hStaticBr;
}

static HBRUSH gui_main_ctlcolor_listbox(HDC hdc) {
    SetTextColor(hdc, COL_TEXT);
    SetBkColor(hdc, COL_INPUT);
    if (!g_hListBr) g_hListBr = CreateSolidBrush(COL_INPUT);
    return g_hListBr;
}

static HBRUSH gui_main_ctlcolor_edit(HDC hdc, HWND control) {
    SetTextColor(hdc, gui_main_input_text_color(control));
    SetBkColor(hdc, COL_INPUT);
    if (!g_hEditBr) g_hEditBr = CreateSolidBrush(COL_INPUT);
    return g_hEditBr;
}
