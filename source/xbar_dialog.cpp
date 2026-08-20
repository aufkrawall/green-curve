// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// xbar_dialog.cpp -- Advanced XBAR clock domain controls (Blackwell only).
// Modal dialog for GPC-to-XBAR clock propagation ratio.
// Included by main_shell.cpp after fan_curve_dialog.cpp.

#define XBAR_DIALOG_CLASS "GreenCurveXbarDialog"
#define XBAR_RATIO_EDIT_ID    3200
#define XBAR_APPLY_BTN_ID     3202
#define XBAR_CLOSE_BTN_ID     3203
#define XBAR_MEASURED_ID      3204
#define XBAR_STOCK_RATIO_ID   3205

// PropRels NVAPI IDs (confirmed on RTX 5070/5090 driver 610.x)
#define PROPRELS_GET_CONTROL_ID  0xCBFF71D0u
#define PROPRELS_SET_CONTROL_ID  0xEF3D20EAu
#define PROPRELS_GET_INFO_ID     0xE826E4F0u
#define PROPRELS_STRUCT_VERSION  0x0001075Cu

struct XbarDialogState {
    HWND hwnd;
    HWND hRatioEdit;
    HWND hMeasuredLabel;
    HWND hStockRatioLabel;
    HWND hApplyBtn;
    HWND hCloseBtn;
    bool active;
    float currentRatio;
};

static XbarDialogState g_xbarDialog = {};

// Read the current PropRels control block
static bool xbar_read_proprels(NvApiFunc getFunc, void* gpuHandle,
                                unsigned char* buf, int bufSize) {
    if (!getFunc || !gpuHandle || !buf || bufSize < 0x100) return false;
    memset(buf, 0, bufSize);
    unsigned int verSize = PROPRELS_STRUCT_VERSION;
    memcpy(buf, &verSize, sizeof(verSize));
    return getFunc(gpuHandle, buf) == 0;
}

// Write a PropRels control block
static bool xbar_write_proprels(NvApiFunc setFunc, void* gpuHandle,
                                 unsigned char* buf, int bufSize) {
    if (!setFunc || !gpuHandle || !buf || bufSize < 0x100) return false;
    return setFunc(gpuHandle, buf) == 0;
}

// Extract the U16.16 ratio from the control block at the relationship-0 slot.
// Layout: version(4) + pad(0x20) + rel_ctrl_entries start at 0x24,
//         stride 0x0c, type-3 ratio at entry+0x08.
// So ratio for relationship 0 is at offset 0x24 + 0x08 = 0x2C.
static float xbar_extract_ratio(const unsigned char* buf) {
    unsigned int raw = 0;
    memcpy(&raw, buf + 0x2C, sizeof(raw));
    return (float)raw / 65536.0f;
}

// Encode a U16.16 ratio
static unsigned int xbar_encode_ratio(float ratio) {
    return (unsigned int)(ratio * 65536.0f + 0.5f);
}

static void xbar_dialog_sync_controls() {
    if (!g_xbarDialog.hwnd) return;
    NvApiFunc getFunc = (NvApiFunc)nvapi_qi(PROPRELS_GET_CONTROL_ID);
    if (!getFunc || !g_app.gpuHandle) return;

    unsigned char buf[0x1000] = {};
    if (xbar_read_proprels(getFunc, g_app.gpuHandle, buf, sizeof(buf))) {
        float ratio = xbar_extract_ratio(buf);
        g_xbarDialog.currentRatio = ratio;
        char buf2[64] = {};
        StringCchPrintfA(buf2, ARRAY_COUNT(buf2), "%.4f", ratio);
        SetWindowTextA(g_xbarDialog.hRatioEdit, buf2);
        StringCchPrintfA(buf2, ARRAY_COUNT(buf2), "Current ratio: %.4f", ratio);
        SetWindowTextA(g_xbarDialog.hStockRatioLabel, buf2);
    }
    // Read measured XBAR clock if available
    unsigned int measuredKhz = g_app.xbarMeasuredClockKhz;
    if (measuredKhz > 0) {
        char buf2[64] = {};
        StringCchPrintfA(buf2, ARRAY_COUNT(buf2), "XBAR: %u MHz", measuredKhz / 1000);
        SetWindowTextA(g_xbarDialog.hMeasuredLabel, buf2);
    }
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
        TextOutA(hdc, dp(12), dp(12), "GPC->XBAR Propagation Ratio:", 29);
        TextOutA(hdc, dp(12), dp(70), "1.0 = stock, 1.2 = ~20% faster XBAR", 37);
        TextOutA(hdc, dp(12), dp(88), "Clock ceiling = core clock x ratio", 34);
        TextOutA(hdc, dp(12), dp(110), "Warning: High ratios may cause", 30);
        TextOutA(hdc, dp(12), dp(126), "instability. Start with 1.05-1.10.", 33);
        SelectObject(hdc, hOld);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case XBAR_APPLY_BTN_ID: {
            if (!g_app.xbarProbeValid) {
                gc_message_box(hwnd, "PropRels control not available.", "Green Curve", MB_OK | MB_ICONWARNING);
                break;
            }
            char ratioBuf[64] = {};
            GetWindowTextA(g_xbarDialog.hRatioEdit, ratioBuf, sizeof(ratioBuf));
            float ratio = 0;
            if (sscanf_s(ratioBuf, "%f", &ratio) != 1 || ratio < 0.5f || ratio > 2.0f) {
                gc_message_box(hwnd, "Invalid ratio. Use 0.5 to 2.0.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            NvApiFunc getFunc = (NvApiFunc)nvapi_qi(PROPRELS_GET_CONTROL_ID);
            NvApiFunc setFunc = (NvApiFunc)nvapi_qi(PROPRELS_SET_CONTROL_ID);
            if (!getFunc || !setFunc) {
                gc_message_box(hwnd, "PropRels NVAPI functions not available.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            // Read current control block
            unsigned char buf[0x1000] = {};
            if (!xbar_read_proprels(getFunc, g_app.gpuHandle, buf, sizeof(buf))) {
                gc_message_box(hwnd, "Failed to read PropRels state.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            // Patch the ratio
            unsigned int rawRatio = xbar_encode_ratio(ratio);
            memcpy(buf + 0x2C, &rawRatio, sizeof(rawRatio));
            // Write
            if (!xbar_write_proprels(setFunc, g_app.gpuHandle, buf, sizeof(buf))) {
                gc_message_box(hwnd, "PropRels SET_CONTROL failed.", "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            // Read back to verify
            unsigned char verifyBuf[0x1000] = {};
            if (xbar_read_proprels(getFunc, g_app.gpuHandle, verifyBuf, sizeof(verifyBuf))) {
                float readbackRatio = xbar_extract_ratio(verifyBuf);
                debug_log("xbar dialog: set ratio %.4f, readback %.4f\n", ratio, readbackRatio);
                g_xbarDialog.currentRatio = readbackRatio;
                char statusMsg[128] = {};
                StringCchPrintfA(statusMsg, ARRAY_COUNT(statusMsg), "Ratio set to %.4f (readback: %.4f)", ratio, readbackRatio);
                SetWindowTextA(g_xbarDialog.hStockRatioLabel, statusMsg);
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
    int dlgW = dp(360), dlgH = dp(210);
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
    int editW = dp(100), editH = dp(22);
    g_xbarDialog.hRatioEdit = CreateWindowExA(0, "EDIT", "1.0",
        WS_CHILD | WS_VISIBLE | ES_RIGHT,
        dp(210), dp(9), editW, editH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_RATIO_EDIT_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hRatioEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hStockRatioLabel = CreateWindowExA(0, "STATIC", "Current ratio: ---",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(12), dp(40), dp(300), dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_STOCK_RATIO_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hStockRatioLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hMeasuredLabel = CreateWindowExA(0, "STATIC", "XBAR: ---",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(12), dp(52), dp(300), dp(18),
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_MEASURED_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hMeasuredLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    int btnW = dp(80), btnH = dp(26);
    g_xbarDialog.hApplyBtn = CreateWindowExA(0, "BUTTON", "Apply",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        dp(12), dp(155), btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_APPLY_BTN_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hApplyBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_xbarDialog.hCloseBtn = CreateWindowExA(0, "BUTTON", "Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        dp(100), dp(155), btnW, btnH,
        g_xbarDialog.hwnd, (HMENU)(INT_PTR)XBAR_CLOSE_BTN_ID, g_app.hInst, nullptr);
    SendMessageA(g_xbarDialog.hCloseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    xbar_dialog_sync_controls();
    ShowWindow(g_xbarDialog.hwnd, SW_SHOW);
}