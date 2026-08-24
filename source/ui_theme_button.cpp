// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Owner-drawn button and checkbox painting for the main window and the fan
// dialog: which control ids are themed, where each checkbox's tick comes from,
// and the button renderer itself.  Split out of main_runtime_ui.cpp (F-MAINT-1),
// which keeps fonts, GDI+ curve drawing, and the themed combo.
//
// Every checkbox here is BS_OWNERDRAW, so Windows stores no check state for it
// and the tick is DERIVED on each paint.  themed_checkbox_painted_state() is the
// mirror that lets the control projections tell whether a repaint is owed; see
// ui_checkbox_state.h for why asking the control instead does not work.

static void draw_checkbox_tick_smooth(HDC hdc, const RECT* box, COLORREF color) {
    if (!hdc || !box) return;

    POINT pts[3] = {
        { box->left + (box->right - box->left) * 22 / 100, box->top + (box->bottom - box->top) * 54 / 100 },
        { box->left + (box->right - box->left) * 44 / 100, box->top + (box->bottom - box->top) * 74 / 100 },
        { box->left + (box->right - box->left) * 78 / 100, box->top + (box->bottom - box->top) * 28 / 100 },
    };

    if (gdiplus_ensure()) {
        void* gfx = nullptr;
        s_fnGfxHDC(hdc, &gfx);
        if (gfx) {
            s_fnSmooth(gfx, GDP_SMOOTH_AA);
            void* pen = nullptr;
            float width = (float)nvmax(2, (box->right - box->left) / 5);
            s_fnMakePen(colorref_to_argb(color), width, GDP_UNIT_PX, &pen);
            if (pen) {
                s_fnLines(gfx, pen, pts, 3);
                s_fnDelPen(pen);
            }
            s_fnDelGfx(gfx);
            return;
        }
    }

    HPEN pen = CreatePen(PS_SOLID, nvmax(2, (box->right - box->left) / 5), color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, pts[0].x, pts[0].y, nullptr);
    LineTo(hdc, pts[1].x, pts[1].y);
    LineTo(hdc, pts[2].x, pts[2].y);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static bool is_themed_button_id(UINT id) {
    switch (id) {
        case APPLY_BTN_ID:
        case REFRESH_BTN_ID:
        case RESET_BTN_ID:
        case LICENSE_BTN_ID:
        case XBAR_ADVANCED_BTN_ID:
        case UPDATE_BTN_ID:
        case PROFILE_LOAD_ID:
        case PROFILE_SAVE_ID:
        case PROFILE_CLEAR_ID:
        case FAN_CURVE_BTN_ID:
        case FAN_DIALOG_OK_ID:
        case FAN_DIALOG_CANCEL_ID:
        case SHARED_PROFILES_BTN_ID:
        case AUTO_PROFILE_BTN_ID:
            return true;
        default:
            break;
    }
    return false;
}

static bool is_themed_checkbox_id(UINT id) {
    return id == START_ON_LOGON_CHECK_ID || id == SERVICE_ENABLE_CHECK_ID ||
           id == SHARE_ALL_USERS_CHECK_ID || is_fan_dialog_checkbox_id(id);
}

static bool is_fan_dialog_checkbox_id(UINT id) {
    return id == FAN_DIALOG_ZERO_RPM_ID ||
        (id >= FAN_DIALOG_ENABLE_BASE &&
         id < FAN_DIALOG_ENABLE_BASE + FAN_CURVE_MAX_POINTS);
}

// The last-painted mirror for the main-window checkboxes whose tick is derived
// at paint time.  The fan dialog checkboxes are not listed on purpose: their
// UiCheckboxState in g_fanCurveDialog IS the source of truth, so there is
// nothing to mirror and they repaint themselves on every toggle.
static UiCheckboxState* themed_checkbox_painted_state(UINT id) {
    if (id == START_ON_LOGON_CHECK_ID) return &g_app.startOnLogonPainted;
    if (id == SERVICE_ENABLE_CHECK_ID) return &g_app.serviceEnablePainted;
    if (id == SHARE_ALL_USERS_CHECK_ID) return &g_app.shareAllUsersPainted;
    return nullptr;
}

static bool themed_checkbox_checked_state(UINT id, HWND hwnd) {
    (void)hwnd;
    if (id == START_ON_LOGON_CHECK_ID) return is_start_on_logon_enabled(g_app.configPath);
    if (id == SERVICE_ENABLE_CHECK_ID) return g_app.backgroundServiceInstalled;
    if (id == SHARE_ALL_USERS_CHECK_ID) {
        // Checked when the selected profile slot is BOTH published to the shared
        // bank AND the current all-users default logon profile (the coherent
        // "shared" state — see share_profile_slot_for_all_users()).
        int sel = g_app.hProfileCombo ? (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0) : -1;
        if (sel < 0 || sel > CONFIG_NUM_SLOTS - 1) sel = CONFIG_DEFAULT_SLOT - 1;
        int slot = sel + 1;
        return is_machine_profile_slot_saved(slot) && g_app.machineLogonSlotCache == slot;
    }
    if (is_fan_dialog_checkbox_id(id)) {
        if (id == FAN_DIALOG_ZERO_RPM_ID) {
            return g_fanCurveDialog.working.zeroRpmEnabled != 0;
        }
        int pointIndex = (int)id - FAN_DIALOG_ENABLE_BASE;
        if (pointIndex >= 0 && pointIndex < FAN_CURVE_MAX_POINTS) {
            return g_fanCurveDialog.working.points[pointIndex].enabled;
        }
    }
    // Unreachable: every is_themed_checkbox_id() id is answered above, and this
    // function is only called for those.  There is no native check state to fall
    // back to on an owner-draw button, so an id arriving here is a wiring bug.
    debug_log("themed checkbox: no checked-state source for control id %u; painting unchecked\n",
        (unsigned)id);
    return false;
}

static void draw_themed_button(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    bool checkbox = is_themed_checkbox_id(dis->CtlID);
    bool checked = checkbox && themed_checkbox_checked_state(dis->CtlID, dis->hwndItem);
    if (checkbox) {
        // Close the loop for the projection repaint gate: what we are about to
        // put on screen becomes the value it compares against.  Recording it
        // here (rather than where the projection runs) keeps the mirror honest
        // even when a full-window RDW_ALLCHILDREN redraw repaints us.
        ui_checkbox_state_set(themed_checkbox_painted_state(dis->CtlID), checked);
        // Owning its caption is what makes a checkbox labeled -- there is no
        // per-id list to keep in sync (F-CHECKBOX-HIT).  The VF lock checkboxes
        // carry no text and are drawn by draw_lock_checkbox() anyway.
        char text[64] = {};
        GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
        draw_themed_checkbox_control(dis, checked, text[0] != 0);
        return;
    }
    HFONT controlFont = dis->hwndItem ? (HFONT)SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0) : nullptr;
    HFONT oldFont = (HFONT)SelectObject(hdc, controlFont ? controlFont : get_ui_font());

    HBRUSH bg = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    COLORREF fill = disabled ? COL_BUTTON_DISABLED : (pressed ? COL_BUTTON_PRESSED : COL_BUTTON);
    HBRUSH fillBr = CreateSolidBrush(fill);
    FillRect(hdc, &rc, fillBr);
    DeleteObject(fillBr);

    // F-PENDING: the fan curve is edited in its own dialog, so this button is
    // the only place its unapplied state can be seen from the main window.
    bool pendingAccent = dis->CtlID == FAN_CURVE_BTN_ID &&
        gui_pending_domain_changed(GUI_PENDING_FAN_CURVE);
    if (dis->CtlID == XBAR_ADVANCED_BTN_ID &&
        (gui_pending_domain_changed(GUI_PENDING_XBAR) ||
         gui_pending_domain_changed(GUI_PENDING_SYS_CLK) ||
         gui_pending_domain_changed(GUI_PENDING_VIDEO_CLK)))
        pendingAccent = true;
    // The same orange, for the same reason, on the button that opens the other
    // dialog that owns state the main window cannot show: "there is something
    // in here you have not dealt with".  Before this the Updates button looked
    // identical whether the machine was current or had a verified installer
    // staged and waiting, so the only passive hint an update existed was a tray
    // menu nobody opens.  Reusing the established colour rather than inventing
    // one keeps the window down to a single "unfinished business" signal.
    if (dis->CtlID == UPDATE_BTN_ID && gui_update_is_available()) pendingAccent = true;
    COLORREF border = disabled ? COL_DISABLED_BORDER : COL_BUTTON_BORDER;
    if (pendingAccent) border = disabled ? COL_PENDING_DIM : COL_PENDING;
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    DeleteObject(SelectObject(hdc, oldPen));

    char text[128] = {};
    GetWindowTextA(dis->hwndItem, text, ARRAY_COUNT(text));
    RECT textRc = rc;
    if (pressed) OffsetRect(&textRc, 0, 1);
    COLORREF labelColor = disabled ? COL_LABEL : COL_BUTTON_LABEL;
    if (pendingAccent) labelColor = disabled ? COL_PENDING_DIM : COL_PENDING;
    SetTextColor(hdc, labelColor);
    DrawTextA(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (focused) {
        RECT focus = rc;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(hdc, &focus);
    }
    SelectObject(hdc, oldFont);
}
