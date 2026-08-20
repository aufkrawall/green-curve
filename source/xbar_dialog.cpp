// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// xbar_dialog.cpp -- Advanced XBAR clock domain controls (Blackwell only).
#include "gpu_backend_xbar.h"
// Modal dialog for XBAR frequency offset and per-domain MSVDD voltage offset.
// Included by main.cpp after fan_curve_dialog.cpp.

#define XBAR_DIALOG_CLASS "GreenCurveXbarDialog"
#define XBAR_OFFSET_EDIT_ID  3200
#define XBAR_MSVDD_EDIT_ID   3201
#define XBAR_APPLY_BTN_ID    3202
#define XBAR_CLOSE_BTN_ID    3203
#define XBAR_MEASURED_ID     3204

struct XbarDialogState {
    HWND hwnd;
    HWND hOffsetEdit;
    HWND hMsvddEdit;
    HWND hMeasuredLabel;
    HWND hApplyBtn;
    HWND hCloseBtn;
    bool active;
};

static XbarDialogState g_xbarDialog = {};

static void xbar_dialog_sync_controls() {
    if (!g_xbarDialog.hwnd) return;
    char buf[32] = {};
    int offsetKhz = g_app.xbarProbeValid ? g_app.xbarFreqOffsetKhz : 0;
    int msvddUv = g_app.xbarProbeValid ? g_app.xbarMsvddOffsetUv : 0;
    unsigned int measuredKhz = g_app.xbarMeasuredClockKhz;
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", offsetKhz);
    SetWindowTextA(g_xbarDialog.hOffsetEdit, buf);
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", msvddUv);
    SetWindowTextA(g_xbarDialog.hMsvddEdit, buf);
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "Measured XBAR: %u kHz (%u MHz)",
        measuredKhz, measuredKhz / 1000);
    SetWindowTextA(g_xbarDialog.hMeasuredLabel, buf);
}

static LRESULT CALLBACK XbarDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_xbarDialog.hwnd = nullptr;
        g_xbarDialog.active = false;
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(32, 32, 32));
        static HBRUSH hbr = CreateSolidBrush(RGB(32, 32, 32));
        return (LRESULT)hbr;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(200, 200, 200));
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hOld = (HFONT)SelectObject(hdc, hFont);
        TextOutA(hdc, dp(12), dp(12), "XBAR Clock Offset (kHz):", 24);
        TextOutA(hdc, dp(12), dp(42), "XBAR MSVDD Offset (uV):", 23);
        TextOutA(hdc, dp(12), dp(82), "Warning: Aggressive offsets may cause", 36);
        TextOutA(hdc, dp(12), dp(98), "visual corruption. Start with small values.", 41);
        SelectObject(hdc, hOld);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case XBAR_APPLY_BTN_ID: {
            if (!g_app.xbarProbeValid) {
                gc_message_box(hwnd, "XBAR control not available on this GPU.", "Green Curve", MB_OK | MB_ICONWARNING);
                break;
            }
            char offsetBuf[32] = {};
            char msvddBuf[32] = {};
            GetWindowTextA(g_xbarDialog.hOffsetEdit, offsetBuf, sizeof(offsetBuf));
            GetWindowTextA(g_xbarDialog.hMsvddEdit, msvddBuf, sizeof(msvddBuf));
            int offsetKhz = 0, msvddUv = 0;
            if (!parse_int_strict(offsetBuf, &offsetKhz) || !parse_int_strict(msvddBuf, &msvddUv)) {
                gc_message_box(hwnd, "Invalid numeric value.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            // Clamp to safe ranges
            if (offsetKhz < -1000000) offsetKhz = -1000000;
            if (offsetKhz > 1000000) offsetKhz = 1000000;
            if (msvddUv < -100000) msvddUv = -100000;
            if (msvddUv > 100000) msvddUv = 100000;
            // Apply via the backend
            NvApiFunc rmGet = (NvApiFunc)nvapi_qi(XBAR_RM_CLK_DOMAINS_GET_CONTROL);
            NvApiFunc rmSet = (NvApiFunc)nvapi_qi(XBAR_RM_CLK_DOMAINS_SET_CONTROL);
            if (!rmGet || !rmSet) {
                gc_message_box(hwnd, "XBAR RM functions not available.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            XbarControlSnapshot snap = {};
            memcpy(snap.buf, g_app.xbarSnapshotBuf, g_app.xbarSnapshotBufSize);
            snap.bufSize = g_app.xbarSnapshotBufSize;
            snap.valid = true;
            if (xbar_write(rmGet, rmSet, g_app.gpuHandle, &snap, offsetKhz, msvddUv)) {
                g_app.xbarFreqOffsetKhz = snap.freqOffsetKhz;
                g_app.xbarMsvddOffsetUv = snap.msvddOffsetUv;
                g_app.xbarMeasuredClockKhz = snap.measuredKhz;
                xbar_dialog_sync_controls();
                debug_log("xbar dialog: applied %d kHz, %d uV, measured %u kHz\n",
                    snap.freqOffsetKhz, snap.msvddOffsetUv, snap.measuredKhz);
            } else {
                gc_message_box(hwnd, "XBAR offset write failed. Check debug log.", "Green Curve", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case XBAR_CLOSE_BTN_ID:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void open_xbar_dialog() {
    if (g_xbarDialog.hwnd) {
        ShowWindow(g_xbarDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_xbarDialog.hwnd);
        xbar_dialog_sync_controls();
        return;
    }
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = XbarDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = XBAR_DIALOG_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExA(&wc);
    int dlgW = dp(340), dlgH = dp(200);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - dlgW) / 2, y = (screenH - dlgH) / 2;
    g_xbarDialog.hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME, XBAR_DIALOG_CLASS, "Advanced XBAR Controls",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, dlgW, dlgH,
        g_app.hMainWnd, nullptr, g_app.hInst, nullptr);
    if (!g_xbarDialog.hwnd) return;
    g_xbarDialog.active = true;
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int editW = dp(120), editH = dp(22);
    g_xbarDialog.hOffsetEdit = CreateWindowExA(0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
        dp(170), dp(9), editW, editH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_OFFSET_EDIT_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hOffsetEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hMsvddEdit = CreateWindowExA(0, "EDIT", "0",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT,
        dp(170), dp(39), editW, editH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_MSVDD_EDIT_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hMsvddEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hMeasuredLabel = CreateWindowExA(0, "STATIC", "Measured XBAR: ---",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(12), dp(66), dp(300), dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_MEASURED_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hMeasuredLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    int btnW = dp(80), btnH = dp(26);
    g_xbarDialog.hApplyBtn = CreateWindowExA(0, "BUTTON", "Apply",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        dp(12), dp(140), btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_APPLY_BTN_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hApplyBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hCloseBtn = CreateWindowExA(0, "BUTTON", "Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        dp(100), dp(140), btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_CLOSE_BTN_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hCloseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    xbar_dialog_sync_controls();
    ShowWindow(g_xbarDialog.hwnd, SW_SHOW);
}