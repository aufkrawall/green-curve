// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "main_layout_policy.h"

enum MainLayoutStaticRole {
    MAIN_STATIC_GPU_LABEL = 0,
    MAIN_STATIC_GPU_OFFSET_LABEL,
    MAIN_STATIC_GPU_EXCLUDE_SUFFIX,
    MAIN_STATIC_MEM_OFFSET_LABEL,
    MAIN_STATIC_POWER_LIMIT_LABEL,
    MAIN_STATIC_FAN_MODE_LABEL,
    MAIN_STATIC_FAN_FIXED_LABEL,
    MAIN_STATIC_ROLE_COUNT,
};

struct MainLayoutControlRegistry {
    HWND pointHeaders[MAIN_LAYOUT_MAX_COLUMNS][3];
    HWND pointLabels[VF_NUM_POINTS];
    HWND statics[MAIN_STATIC_ROLE_COUNT];
};

static MainLayoutControlRegistry s_mainLayoutControls = {};
static MainLayoutPlan s_mainLayoutPlan = {};
static int s_mainLayoutScrollX = 0;
static int s_mainLayoutScrollY = 0;
static int s_mainLayoutWheelRemainderX = 0;
static int s_mainLayoutWheelRemainderY = 0;
static bool s_mainLayoutInProgress = false;
static bool s_mainLayoutHasPlan = false;
static bool s_mainLayoutPendingContentGrowth = false;
static int s_mainLayoutPendingPointCount = 0;
static const UINT_PTR MAIN_LAYOUT_FOCUS_SUBCLASS_ID = 0x47434C59u;

static DWORD main_window_style() {
    return WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL;
}

static SIZE adjusted_window_size_for_client(
    int clientWidth, int clientHeight, DWORD style, DWORD exStyle) {
    RECT rc = { 0, 0, clientWidth, clientHeight };
    typedef BOOL (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpi_t adjustForDpi =
        (AdjustWindowRectExForDpi_t)GetProcAddress(
            GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
    if (adjustForDpi) {
        adjustForDpi(&rc, style, FALSE, exStyle, (UINT)g_dpi);
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    SIZE size = { rc.right - rc.left, rc.bottom - rc.top };
    return size;
}

static RECT main_window_work_area_for_rect(const RECT* rect) {
    POINT center = {};
    if (rect) {
        center.x = rect->left + (rect->right - rect->left) / 2;
        center.y = rect->top + (rect->bottom - rect->top) / 2;
    }
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoA(monitor, &info)) return info.rcWork;
    RECT work = {};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

static RECT main_window_work_area(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoA(monitor, &info)) return info.rcWork;
    RECT work = {};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

static RECT clamp_window_rect_to_work_area(RECT window, RECT work) {
    int workW = main_layout_max_int(1, work.right - work.left);
    int workH = main_layout_max_int(1, work.bottom - work.top);
    int width = main_layout_min_int(main_layout_max_int(1, window.right - window.left), workW);
    int height = main_layout_min_int(main_layout_max_int(1, window.bottom - window.top), workH);
    int left = main_layout_clamp_int(window.left, work.left, work.right - width);
    int top = main_layout_clamp_int(window.top, work.top, work.bottom - height);
    RECT result = { left, top, left + width, top + height };
    return result;
}

static RECT main_layout_rect_to_win32(MainLayoutRect value) {
    RECT result = { value.left, value.top, value.right, value.bottom };
    return result;
}

static MainLayoutRect main_layout_rect_from_win32(RECT value) {
    MainLayoutRect result = { value.left, value.top, value.right, value.bottom };
    return result;
}

static SIZE main_window_initial_size() {
    const int clientW = dp(MAIN_LAYOUT_BASE_WIDTH_LOGICAL);
    const int clientH = main_layout_preferred_client_height(clientW, g_dpi, g_app.numVisible);
    SIZE desired = adjusted_window_size_for_client(clientW, clientH, main_window_style(), 0);
    RECT work = {};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    desired.cx = main_layout_min_int(desired.cx, main_layout_max_int(1, work.right - work.left));
    desired.cy = main_layout_min_int(desired.cy, main_layout_max_int(1, work.bottom - work.top));
    return desired;
}

static RECT main_window_initial_rect(SIZE initialSize, bool* restoredOut) {
    if (restoredOut) *restoredOut = false;
    int version = get_config_int(
        g_app.configPath, "ui", "main_window_placement_version", 0);
    int left = get_config_int(g_app.configPath, "ui", "main_window_left", 0);
    int top = get_config_int(g_app.configPath, "ui", "main_window_top", 0);
    int width = get_config_int(g_app.configPath, "ui", "main_window_width", 0);
    int height = get_config_int(g_app.configPath, "ui", "main_window_height", 0);
    bool validStored = version == 1 && left >= -100000 && left <= 100000 &&
        top >= -100000 && top <= 100000 && width >= 320 && width <= 32768 &&
        height >= 240 && height <= 32768;
    if (validStored) {
        RECT stored = { left, top, left + width, top + height };
        RECT work = main_window_work_area_for_rect(&stored);
        RECT target = clamp_window_rect_to_work_area(stored, work);
        if (restoredOut) *restoredOut = true;
        debug_log("main window startup placement: source=stored raw=%d,%d %dx%d work=%ld,%ld-%ld,%ld target=%ld,%ld %ldx%ld\n",
            left, top, width, height, work.left, work.top, work.right, work.bottom,
            target.left, target.top, target.right - target.left,
            target.bottom - target.top);
        return target;
    }

    POINT cursor = {};
    if (!GetCursorPos(&cursor)) {
        cursor.x = GetSystemMetrics(SM_CXSCREEN) / 2;
        cursor.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    }
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT work = {};
    if (!monitor || !GetMonitorInfoA(monitor, &info)) {
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    } else {
        work = info.rcWork;
    }
    MainLayoutRect centered = main_layout_center_rect(
        main_layout_rect_from_win32(work), initialSize.cx, initialSize.cy);
    RECT target = main_layout_rect_to_win32(centered);
    debug_log("main window startup placement: source=centered cursor=%ld,%ld work=%ld,%ld-%ld,%ld target=%ld,%ld %ldx%ld storedVersion=%d\n",
        cursor.x, cursor.y, work.left, work.top, work.right, work.bottom,
        target.left, target.top, target.right - target.left,
        target.bottom - target.top, version);
    return target;
}

static bool persist_main_window_placement(HWND hwnd) {
    if (!hwnd || !g_app.configPath[0]) return false;
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        debug_log("main window placement save failed: GetWindowPlacement error=%lu\n",
            (unsigned long)GetLastError());
        return false;
    }
    RECT normal = placement.rcNormalPosition;
    int width = normal.right - normal.left;
    int height = normal.bottom - normal.top;
    bool valid = width >= 320 && width <= 32768 &&
        height >= 240 && height <= 32768;
    bool ok = valid &&
        set_config_int(g_app.configPath, "ui", "main_window_placement_version", 0) &&
        set_config_int(g_app.configPath, "ui", "main_window_left", normal.left) &&
        set_config_int(g_app.configPath, "ui", "main_window_top", normal.top) &&
        set_config_int(g_app.configPath, "ui", "main_window_width", width) &&
        set_config_int(g_app.configPath, "ui", "main_window_height", height) &&
        set_config_int(g_app.configPath, "ui", "main_window_placement_version", 1);
    debug_log("main window placement save: normal=%ld,%ld %dx%d showCmd=%u valid=%d committed=%d\n",
        normal.left, normal.top, width, height, placement.showCmd,
        valid ? 1 : 0, ok ? 1 : 0);
    return ok;
}

static SIZE main_window_min_track_size(HWND hwnd) {
    SIZE desired = adjusted_window_size_for_client(
        dp(MAIN_LAYOUT_MIN_VIEWPORT_WIDTH_LOGICAL),
        dp(MAIN_LAYOUT_MIN_VIEWPORT_HEIGHT_LOGICAL), main_window_style(), 0);
    RECT work = main_window_work_area(hwnd);
    desired.cx = main_layout_min_int(desired.cx, main_layout_max_int(1, work.right - work.left));
    desired.cy = main_layout_min_int(desired.cy, main_layout_max_int(1, work.bottom - work.top));
    return desired;
}

static int main_layout_window_dpi(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
    static GetDpiForWindow_t getDpiForWindow = (GetDpiForWindow_t)GetProcAddress(
        GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT dpi = getDpiForWindow && hwnd ? getDpiForWindow(hwnd) : (UINT)g_dpi;
    return dpi >= 48 && dpi <= 768 ? (int)dpi : g_dpi;
}

static void main_layout_register_point_header(int column, int field, HWND hwnd) {
    if (column < 0 || column >= MAIN_LAYOUT_MAX_COLUMNS || field < 0 || field >= 3) return;
    s_mainLayoutControls.pointHeaders[column][field] = hwnd;
}

static void main_layout_register_point_label(int visibleIndex, HWND hwnd) {
    if (visibleIndex < 0 || visibleIndex >= VF_NUM_POINTS) return;
    s_mainLayoutControls.pointLabels[visibleIndex] = hwnd;
}

static void main_layout_register_static(MainLayoutStaticRole role, HWND hwnd) {
    if (role < 0 || role >= MAIN_STATIC_ROLE_COUNT) return;
    s_mainLayoutControls.statics[role] = hwnd;
}

static void main_layout_forget_dynamic_controls() {
    memset(&s_mainLayoutControls, 0, sizeof(s_mainLayoutControls));
}

static LRESULT CALLBACK main_layout_focus_subclass_proc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (msg == WM_SETFOCUS) {
        HWND parent = GetParent(hwnd);
        if (parent) PostMessageA(parent, APP_WM_ENSURE_LAYOUT_FOCUS, (WPARAM)hwnd, 0);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, main_layout_focus_subclass_proc, subclassId);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void main_layout_install_focus_subclasses(HWND parent) {
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        SetWindowSubclass(child, main_layout_focus_subclass_proc,
            MAIN_LAYOUT_FOCUS_SUBCLASS_ID, 0);
    }
}

struct MainLayoutMoveBatch {
    HDWP defer;
    int scrollX;
    int scrollY;
    bool failed;
};

static void main_layout_move(MainLayoutMoveBatch* batch, HWND hwnd,
    int x, int y, int width, int height, bool show = true) {
    if (!batch || !hwnd) return;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    if (batch->defer) {
        HDWP next = DeferWindowPos(batch->defer, hwnd, nullptr,
            x - batch->scrollX, y - batch->scrollY,
            main_layout_max_int(1, width), main_layout_max_int(1, height), flags);
        if (next) {
            batch->defer = next;
        } else {
            batch->defer = nullptr;
            batch->failed = true;
            SetWindowPos(hwnd, nullptr, x - batch->scrollX, y - batch->scrollY,
                main_layout_max_int(1, width), main_layout_max_int(1, height), flags);
        }
    } else {
        SetWindowPos(hwnd, nullptr, x - batch->scrollX, y - batch->scrollY,
            main_layout_max_int(1, width), main_layout_max_int(1, height), flags);
    }
}

static void main_layout_update_scroll_info(HWND hwnd, const MainLayoutPlan& plan) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    int viewportW = main_layout_max_int(1, client.right - client.left);
    int viewportH = main_layout_max_int(1, client.bottom - client.top);
    int maxX = main_layout_max_int(0, plan.contentWidth - viewportW);
    int maxY = main_layout_max_int(0, plan.contentHeight - viewportH);
    s_mainLayoutScrollX = main_layout_clamp_int(s_mainLayoutScrollX, 0, maxX);
    s_mainLayoutScrollY = main_layout_clamp_int(s_mainLayoutScrollY, 0, maxY);

    SCROLLINFO horizontal = {};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    horizontal.nMin = 0;
    horizontal.nMax = main_layout_max_int(0, plan.contentWidth - 1);
    horizontal.nPage = (UINT)viewportW;
    horizontal.nPos = s_mainLayoutScrollX;
    SetScrollInfo(hwnd, SB_HORZ, &horizontal, TRUE);

    SCROLLINFO vertical = {};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    vertical.nMin = 0;
    vertical.nMax = main_layout_max_int(0, plan.contentHeight - 1);
    vertical.nPage = (UINT)viewportH;
    vertical.nPos = s_mainLayoutScrollY;
    SetScrollInfo(hwnd, SB_VERT, &vertical, TRUE);
    ShowScrollBar(hwnd, SB_HORZ, maxX > 0);
    ShowScrollBar(hwnd, SB_VERT, maxY > 0);
}

static void main_layout_place_controls(
    HWND parent, const MainLayoutPlan& plan, bool forceDirect = false) {
    MainLayoutMoveBatch batch = {
        forceDirect ? nullptr : BeginDeferWindowPos(420),
        s_mainLayoutScrollX, s_mainLayoutScrollY, false
    };
    const int cbW = dp(16);
    const int editW = dp(65);
    const int labelW = dp(32);
    const int gap = dp(2);
    const int rowH = dp(20);
    const int headerH = dp(16);
    const int colW = dp(MAIN_LAYOUT_COLUMN_WIDTH_LOGICAL);

    for (int col = 0; col < MAIN_LAYOUT_MAX_COLUMNS; ++col) {
        int x = dp(8) + col * colW;
        int y = plan.pointStartY - headerH - dp(2);
        bool show = col < plan.columns;
        main_layout_move(&batch, s_mainLayoutControls.pointHeaders[col][0],
            x + labelW + gap, y, cbW, headerH, show);
        main_layout_move(&batch, s_mainLayoutControls.pointHeaders[col][1],
            x + labelW + cbW + gap * 2, y, editW, headerH, show);
        main_layout_move(&batch, s_mainLayoutControls.pointHeaders[col][2],
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, headerH, show);
    }

    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        MainLayoutPointCell cell = main_layout_point_cell(plan, vi);
        int x = cell.left;
        int y = cell.top;
        main_layout_move(&batch, s_mainLayoutControls.pointLabels[vi],
            x, y + dp(1), labelW - gap, rowH - dp(2));
        main_layout_move(&batch, g_app.hLocks[vi],
            x + labelW, y + dp(1), cbW, rowH - dp(2));
        main_layout_move(&batch, g_app.hEditsMhz[vi],
            x + labelW + cbW + gap * 2, y, editW, rowH - dp(2));
        main_layout_move(&batch, g_app.hEditsMv[vi],
            x + labelW + cbW + gap * 2 + editW + gap, y, editW, rowH - dp(2));
    }

    int gpuSelectW = dp(420);
    int gpuSelectX = plan.contentWidth - gpuSelectW - dp(12);
    int gpuSelectY = dp(10);
    int gpuLabelW = dp(42);
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_GPU_LABEL],
        gpuSelectX - gpuLabelW - dp(6), gpuSelectY + dp(3), gpuLabelW, dp(18));
    main_layout_move(&batch, g_app.hGpuSelectCombo,
        gpuSelectX, gpuSelectY, gpuSelectW, dp(220));

    int ocY = plan.globalControlsY;
    int fieldW = dp(78);
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_GPU_OFFSET_LABEL],
        dp(8), ocY + dp(2), dp(126), dp(18));
    main_layout_move(&batch, g_app.hGpuOffsetEdit,
        dp(136), ocY, fieldW, dp(20));
    main_layout_move(&batch, g_app.hGpuOffsetExcludeLowLabel,
        dp(8), ocY + dp(25), dp(76), dp(18));
    main_layout_move(&batch, g_app.hGpuOffsetExcludeLowEdit,
        dp(86), ocY + dp(23), dp(50), dp(20));
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_GPU_EXCLUDE_SUFFIX],
        dp(140), ocY + dp(25), dp(70), dp(18));
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_MEM_OFFSET_LABEL],
        dp(230), ocY + dp(2), dp(126), dp(18));
    main_layout_move(&batch, g_app.hMemOffsetEdit,
        dp(358), ocY, fieldW, dp(20));
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_POWER_LIMIT_LABEL],
        dp(452), ocY + dp(2), dp(100), dp(18));
    main_layout_move(&batch, g_app.hPowerLimitEdit,
        dp(552), ocY, fieldW, dp(20));
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_FAN_MODE_LABEL],
        dp(650), ocY + dp(2), dp(88), dp(18));
    main_layout_move(&batch, g_app.hFanModeCombo,
        dp(738), ocY, dp(136), dp(220));
    main_layout_move(&batch, s_mainLayoutControls.statics[MAIN_STATIC_FAN_FIXED_LABEL],
        dp(882), ocY + dp(2), dp(58), dp(18));
    main_layout_move(&batch, g_app.hFanEdit,
        dp(942), ocY, dp(56), dp(20));
    main_layout_move(&batch, g_app.hFanCurveBtn,
        dp(1006), ocY - dp(1), dp(160), dp(24));

    const int margin = dp(8);
    const int buttonH = dp(30);
    const int smallButtonW = dp(76);
    const int smallGap = dp(6);
    const int comboDropH = dp(220);
    main_layout_move(&batch, g_app.hApplyBtn, margin, plan.buttonsY, dp(132), buttonH);
    main_layout_move(&batch, g_app.hRefreshBtn, margin + dp(144), plan.buttonsY, dp(98), buttonH);
    main_layout_move(&batch, g_app.hResetBtn, margin + dp(254), plan.buttonsY, dp(98), buttonH);
    main_layout_move(&batch, g_app.hLicenseBtn,
        plan.contentWidth - margin - dp(118), plan.buttonsY, dp(118), buttonH);

    main_layout_move(&batch, g_app.hProfileLabel,
        margin, plan.profileY + dp(4), dp(72), dp(18));
    main_layout_move(&batch, g_app.hProfileCombo,
        margin + dp(76), plan.profileY, dp(156), comboDropH);
    main_layout_move(&batch, g_app.hProfileLoadBtn,
        margin + dp(244), plan.profileY, smallButtonW, dp(28));
    main_layout_move(&batch, g_app.hProfileSaveBtn,
        margin + dp(244) + smallButtonW + smallGap, plan.profileY, smallButtonW, dp(28));
    main_layout_move(&batch, g_app.hProfileClearBtn,
        margin + dp(244) + (smallButtonW + smallGap) * 2,
        plan.profileY, smallButtonW, dp(28));
    int stateX = margin + dp(244) + (smallButtonW + smallGap) * 3 + dp(12);
    main_layout_move(&batch, g_app.hProfileStateLabel,
        stateX, plan.profileY + dp(4),
        main_layout_max_int(dp(140), plan.contentWidth - stateX - margin), dp(18));

    main_layout_move(&batch, g_app.hAppLaunchLabel,
        margin, plan.autoY + dp(4), dp(170), dp(18));
    main_layout_move(&batch, g_app.hAppLaunchCombo,
        margin + dp(174), plan.autoY, dp(170), comboDropH);
    main_layout_move(&batch, g_app.hLogonLabel,
        margin + dp(366), plan.autoY + dp(4), dp(208), dp(18));
    main_layout_move(&batch, g_app.hLogonCombo,
        margin + dp(578), plan.autoY, dp(170), comboDropH);
    main_layout_move(&batch, g_app.hStartOnLogonCheck,
        margin + dp(760), plan.autoY + dp(4), dp(16), dp(16));
    main_layout_move(&batch, g_app.hStartOnLogonLabel,
        margin + dp(784), plan.autoY + dp(3), dp(200), dp(18));
    main_layout_move(&batch, g_app.hShareAllUsersCheck,
        margin, plan.sharedY, dp(280), dp(22));
    main_layout_move(&batch, g_app.hSharedProfilesBtn,
        margin + dp(300), plan.sharedY, dp(150), dp(22));
    main_layout_move(&batch, g_app.hAutoProfilesBtn,
        margin + dp(460), plan.sharedY, dp(160), dp(22));
    main_layout_move(&batch, g_app.hServiceEnableCheck,
        margin, plan.serviceY + dp(4), dp(16), dp(16));
    main_layout_move(&batch, g_app.hServiceEnableLabel,
        margin + dp(24), plan.serviceY + dp(3), dp(330), dp(18));
    main_layout_move(&batch, g_app.hServiceStatusLabel,
        margin + dp(370), plan.serviceY + dp(3),
        main_layout_max_int(dp(220), plan.contentWidth - margin - dp(370)), dp(18));
    main_layout_move(&batch, g_app.hLogonHintLabel,
        margin, plan.hintY,
        main_layout_max_int(dp(320), plan.contentWidth - margin * 2), dp(34));
    main_layout_move(&batch, g_app.hProfileStatusLabel,
        margin, plan.statusY,
        main_layout_max_int(dp(300), plan.contentWidth - margin * 2), dp(18));

    if (batch.defer && !EndDeferWindowPos(batch.defer)) batch.failed = true;
    if (batch.failed && !forceDirect) {
        debug_log("main layout: DeferWindowPos batch failed; retrying complete placement directly\n");
        main_layout_place_controls(parent, plan, true);
    }
}

static void layout_main_window(HWND hwnd) {
    if (!hwnd || s_mainLayoutInProgress) return;
    s_mainLayoutInProgress = true;

    MainLayoutPlan plan = {};
    int previousW = -1;
    int previousH = -1;
    // A scrollbar changes the perpendicular client dimension and can make the
    // other scrollbar necessary. Iterate to the stable viewport instead of
    // assuming one pass is sufficient.
    for (int pass = 0; pass < 4; ++pass) {
        RECT client = {};
        GetClientRect(hwnd, &client);
        int width = main_layout_max_int(1, client.right);
        int height = main_layout_max_int(1, client.bottom);
        plan = main_layout_build_plan(width, height, g_dpi, g_app.numVisible);
        main_layout_update_scroll_info(hwnd, plan);
        RECT after = {};
        GetClientRect(hwnd, &after);
        if (after.right == width && after.bottom == height &&
            width == previousW && height == previousH) break;
        previousW = width;
        previousH = height;
    }
    s_mainLayoutPlan = plan;
    s_mainLayoutHasPlan = true;
    main_layout_place_controls(hwnd, plan);
    main_layout_install_focus_subclasses(hwnd);

    static int lastDpi = 0, lastW = 0, lastH = 0, lastContentW = 0;
    static int lastContentH = 0, lastGraphH = 0, lastColumns = 0;
    static bool lastOverflowX = false, lastOverflowY = false;
    bool changed = lastDpi != g_dpi || lastW != plan.viewportWidth ||
        lastH != plan.viewportHeight || lastContentW != plan.contentWidth ||
        lastContentH != plan.contentHeight || lastGraphH != plan.graphHeight ||
        lastColumns != plan.columns || lastOverflowX != plan.horizontalOverflow ||
        lastOverflowY != plan.verticalOverflow;
    if (changed) {
        debug_log("main layout: dpi=%d viewport=%dx%d content=%dx%d graphH=%d columns=%d rows=%d overflowX=%d overflowY=%d scroll=%d,%d\n",
            g_dpi, plan.viewportWidth, plan.viewportHeight,
            plan.contentWidth, plan.contentHeight, plan.graphHeight,
            plan.columns, plan.rowsPerColumn,
            plan.horizontalOverflow ? 1 : 0, plan.verticalOverflow ? 1 : 0,
            s_mainLayoutScrollX, s_mainLayoutScrollY);
        lastDpi = g_dpi; lastW = plan.viewportWidth; lastH = plan.viewportHeight;
        lastContentW = plan.contentWidth; lastContentH = plan.contentHeight;
        lastGraphH = plan.graphHeight; lastColumns = plan.columns;
        lastOverflowX = plan.horizontalOverflow; lastOverflowY = plan.verticalOverflow;
    }
    s_mainLayoutInProgress = false;
}

static bool main_layout_grow_window_for_content(
    HWND hwnd, int pointCount, const char* reason) {
    if (!hwnd || pointCount <= 0) return false;
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        s_mainLayoutPendingContentGrowth = true;
        s_mainLayoutPendingPointCount = pointCount;
        debug_log("main content growth deferred: reason=%s points=%d iconic=%d zoomed=%d\n",
            reason ? reason : "unspecified", pointCount,
            IsIconic(hwnd) ? 1 : 0, IsZoomed(hwnd) ? 1 : 0);
        return false;
    }

    s_mainLayoutPendingContentGrowth = false;
    s_mainLayoutPendingPointCount = 0;
    RECT client = {};
    RECT window = {};
    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window);
    RECT work = main_window_work_area(hwnd);
    int desiredClientWidth = main_layout_max_int(
        main_layout_max_int(1, client.right), dp(MAIN_LAYOUT_BASE_WIDTH_LOGICAL));
    int desiredClientHeight = main_layout_preferred_client_height(
        desiredClientWidth, g_dpi, pointCount);
    SIZE preferredOuter = adjusted_window_size_for_client(
        desiredClientWidth, desiredClientHeight, main_window_style(), 0);
    MainLayoutSize targetSize = main_layout_grow_size(
        window.right - window.left, window.bottom - window.top,
        preferredOuter.cx, preferredOuter.cy,
        work.right - work.left, work.bottom - work.top);
    MainLayoutRect centeredCandidate = main_layout_resize_around_center(
        main_layout_rect_from_win32(window),
        targetSize.width, targetSize.height);
    RECT candidate = main_layout_rect_to_win32(centeredCandidate);
    RECT target = clamp_window_rect_to_work_area(candidate, work);
    bool changed = !EqualRect(&window, &target);
    debug_log("main content growth: reason=%s points=%d dpi=%d client=%ldx%ld window=%ldx%ld preferredClient=%dx%d preferredOuter=%dx%d work=%ldx%ld target=%ld,%ld %ldx%ld changed=%d\n",
        reason ? reason : "unspecified", pointCount, g_dpi,
        client.right, client.bottom,
        window.right - window.left, window.bottom - window.top,
        desiredClientWidth, desiredClientHeight,
        preferredOuter.cx, preferredOuter.cy,
        work.right - work.left, work.bottom - work.top,
        target.left, target.top, target.right - target.left, target.bottom - target.top,
        changed ? 1 : 0);
    if (changed) {
        SetWindowPos(hwnd, nullptr, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout_main_window(hwnd);
    return changed;
}

static void main_layout_apply_pending_content_growth(HWND hwnd) {
    if (!s_mainLayoutPendingContentGrowth || !hwnd || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    int pointCount = s_mainLayoutPendingPointCount;
    s_mainLayoutPendingContentGrowth = false;
    s_mainLayoutPendingPointCount = 0;
    main_layout_grow_window_for_content(hwnd, pointCount, "deferred restore");
}

static int main_layout_graph_height() {
    return s_mainLayoutHasPlan ? s_mainLayoutPlan.graphHeight : dp(GRAPH_HEIGHT);
}

static int main_layout_content_width() {
    return s_mainLayoutHasPlan ? s_mainLayoutPlan.contentWidth : dp(WINDOW_WIDTH);
}

static int main_layout_scroll_x() { return s_mainLayoutScrollX; }
static int main_layout_scroll_y() { return s_mainLayoutScrollY; }

static bool main_layout_set_scroll(HWND hwnd, int x, int y) {
    if (!s_mainLayoutHasPlan) return false;
    RECT client = {};
    GetClientRect(hwnd, &client);
    int maxX = main_layout_max_int(0, s_mainLayoutPlan.contentWidth - client.right);
    int maxY = main_layout_max_int(0, s_mainLayoutPlan.contentHeight - client.bottom);
    x = main_layout_clamp_int(x, 0, maxX);
    y = main_layout_clamp_int(y, 0, maxY);
    if (x == s_mainLayoutScrollX && y == s_mainLayoutScrollY) return false;
    int oldX = s_mainLayoutScrollX;
    int oldY = s_mainLayoutScrollY;
    s_mainLayoutScrollX = x;
    s_mainLayoutScrollY = y;
    debug_log("main layout scroll: from=%d,%d to=%d,%d viewport=%dx%d content=%dx%d\n",
        oldX, oldY, x, y, client.right, client.bottom,
        s_mainLayoutPlan.contentWidth, s_mainLayoutPlan.contentHeight);
    SCROLLINFO horizontal = {};
    horizontal.cbSize = sizeof(horizontal);
    horizontal.fMask = SIF_POS;
    horizontal.nPos = x;
    SetScrollInfo(hwnd, SB_HORZ, &horizontal, TRUE);
    SCROLLINFO vertical = {};
    vertical.cbSize = sizeof(vertical);
    vertical.fMask = SIF_POS;
    vertical.nPos = y;
    SetScrollInfo(hwnd, SB_VERT, &vertical, TRUE);

    // Move the existing window pixels and child HWNDs as one scroll operation.
    // Re-running hundreds of SetWindowPos calls here allowed DWM to present a
    // transient mixture of stale backbuffer pixels and newly moved controls.
    // Erase the exposed area and synchronously repaint the complete parent plus
    // children before returning from the input message.
    ScrollWindowEx(hwnd, oldX - x, oldY - y,
        nullptr, nullptr, nullptr, nullptr,
        SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return true;
}

static bool main_layout_handle_scroll(HWND hwnd, int bar, UINT code) {
    SCROLLINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(hwnd, bar, &info);
    int current = bar == SB_HORZ ? s_mainLayoutScrollX : s_mainLayoutScrollY;
    int line = dp(40);
    int page = main_layout_max_int(line, (int)info.nPage - line);
    int next = current;
    switch (code) {
        case SB_LINEUP: next -= line; break;
        case SB_LINEDOWN: next += line; break;
        case SB_PAGEUP: next -= page; break;
        case SB_PAGEDOWN: next += page; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: next = info.nTrackPos; break;
        case SB_TOP: next = info.nMin; break;
        case SB_BOTTOM: next = info.nMax; break;
        default: return false;
    }
    return bar == SB_HORZ
        ? main_layout_set_scroll(hwnd, next, s_mainLayoutScrollY)
        : main_layout_set_scroll(hwnd, s_mainLayoutScrollX, next);
}

static bool main_layout_handle_mouse_wheel(HWND hwnd, short delta, bool horizontal) {
    int* remainder = horizontal
        ? &s_mainLayoutWheelRemainderX : &s_mainLayoutWheelRemainderY;
    *remainder += (int)delta;
    int detents = *remainder / WHEEL_DELTA;
    *remainder %= WHEEL_DELTA;
    if (detents == 0) return false;
    int amount = detents * dp(120);
    return horizontal
        ? main_layout_set_scroll(hwnd, s_mainLayoutScrollX + amount, s_mainLayoutScrollY)
        : main_layout_set_scroll(hwnd, s_mainLayoutScrollX, s_mainLayoutScrollY - amount);
}

static void main_layout_ensure_child_visible(HWND hwnd, HWND child) {
    if (!hwnd || !child || GetParent(child) != hwnd || !s_mainLayoutHasPlan) return;
    RECT childRect = {};
    GetWindowRect(child, &childRect);
    MapWindowPoints(nullptr, hwnd, (POINT*)&childRect, 2);
    RECT client = {};
    GetClientRect(hwnd, &client);
    int margin = dp(8);
    int newX = s_mainLayoutScrollX;
    int newY = s_mainLayoutScrollY;
    if (childRect.left < margin) newX += childRect.left - margin;
    else if (childRect.right > client.right - margin)
        newX += childRect.right - (client.right - margin);
    if (childRect.top < margin) newY += childRect.top - margin;
    else if (childRect.bottom > client.bottom - margin)
        newY += childRect.bottom - (client.bottom - margin);
    main_layout_set_scroll(hwnd, newX, newY);
}

static void reset_main_window_dpi_resources(HWND hwnd, int newDpi) {
    if (newDpi < 48 || newDpi > 768 || newDpi == g_dpi) return;
    int oldDpi = g_dpi;
    HFONT oldUiFont = s_hUiFont;
    s_hUiFont = nullptr;
    s_uiBaseLogFontReady = false;
    memset(&s_uiBaseLogFont, 0, sizeof(s_uiBaseLogFont));
    g_dpi = newDpi;
    g_scale = (float)newDpi / 96.0f;
    apply_ui_font_to_children(hwnd);
    if (oldUiFont) DeleteObject(oldUiFont);
    if (g_app.hCachedFont) { DeleteObject(g_app.hCachedFont); g_app.hCachedFont = nullptr; }
    if (g_app.hCachedFontSmall) { DeleteObject(g_app.hCachedFontSmall); g_app.hCachedFontSmall = nullptr; }
    g_app.hCachedFont = CreateFontA(dp(13), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_app.hCachedFontSmall = CreateFontA(dp(11), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    debug_log("main DPI changed: old=%d new=%d scale=%.3f; fonts and layout rebuilt\n",
        oldDpi, newDpi, g_scale);
}

static void main_layout_handle_dpi_changed(HWND hwnd, int newDpi, const RECT* suggested) {
    reset_main_window_dpi_resources(hwnd, newDpi);
    if (suggested) {
        RECT work = main_window_work_area_for_rect(suggested);
        RECT target = clamp_window_rect_to_work_area(*suggested, work);
        SetWindowPos(hwnd, nullptr, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout_main_window(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void ensure_main_window_fits_work_area(HWND hwnd) {
    if (!hwnd) return;
    RECT window = {};
    GetWindowRect(hwnd, &window);
    RECT work = main_window_work_area(hwnd);
    RECT target = clamp_window_rect_to_work_area(window, work);
    if (!EqualRect(&window, &target)) {
        debug_log("main window clamped to work area: from=%ld,%ld %ldx%ld work=%ld,%ld-%ld,%ld to=%ld,%ld %ldx%ld\n",
            window.left, window.top, window.right - window.left, window.bottom - window.top,
            work.left, work.top, work.right, work.bottom,
            target.left, target.top, target.right - target.left, target.bottom - target.top);
        SetWindowPos(hwnd, nullptr, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout_main_window(hwnd);
}

static void layout_bottom_buttons(HWND hwnd) {
    layout_main_window(hwnd);
}
