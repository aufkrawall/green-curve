// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// auto_profile_dialog.cpp — the "Configure auto-profiles" editor.  A pseudo-modal
// window (parent disabled while open) mirroring fan_curve_dialog.cpp: global
// settings + a fixed 8-row rule grid (ordered, first match wins) + per-slot
// global hotkeys.  On Save it persists via auto_profile_config_save() + the
// [hotkeys] section and calls auto_profile_reload_config() so the running driver
// picks up the change.

#include "app_shared.h"
#include "auto_profile.h"
#include "hotkeys.h"
#include "ui_checkbox_state.h"

// Dialog-local control ids (its own WndProc, so no clash with main-window ids).
#define APD_ENABLE_CHECK_ID    4000
#define APD_DEFAULT_SLOT_COMBO 4001
#define APD_DEBOUNCE_EDIT      4002
#define APD_COOLDOWN_EDIT      4003
#define APD_SUPPRESS_CHECK     4004
#define APD_RULE_TYPE_BASE     4100   // + rule index
#define APD_RULE_PATTERN_BASE  4120
#define APD_RULE_FOCUS_BASE    4140
#define APD_RULE_SLOT_BASE     4160
#define APD_HOTKEY_BASE        4200   // + slot (1..CONFIG_NUM_SLOTS)
#define APD_SAVE_ID            4300
#define APD_CANCEL_ID          4301

struct AutoProfileDialogState {
    HWND hwnd;
    HWND enableCheck, defaultSlotCombo, debounceEdit, cooldownEdit, suppressCheck;
    HWND typeCombo[AUTO_PROFILE_MAX_RULES];
    HWND patternEdit[AUTO_PROFILE_MAX_RULES];
    HWND focusCheck[AUTO_PROFILE_MAX_RULES];
    HWND slotCombo[AUTO_PROFILE_MAX_RULES];
    HWND hotkeyEdit[CONFIG_NUM_SLOTS + 1];   // 1-based by slot
    HWND saveButton, cancelButton;
    HBRUSH hEditBrush, hStaticBrush, hBtnBrush, hListBrush;
    UiCheckboxState enableState;
    UiCheckboxState suppressState;
    UiCheckboxState focusState[AUTO_PROFILE_MAX_RULES];
};
static AutoProfileDialogState g_apDialog;

static int apd_combo_data(HWND combo, int fallback) {
    if (!combo) return fallback;
    int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return fallback;
    LRESULT v = SendMessageA(combo, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (v == CB_ERR) return fallback;
    return (int)v;
}

static void apd_combo_select(HWND combo, int value) {
    if (!combo) return;
    int n = (int)SendMessageA(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        if ((int)SendMessageA(combo, CB_GETITEMDATA, (WPARAM)i, 0) == value) {
            SendMessageA(combo, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    if (n > 0) SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

static void apd_combo_add(HWND combo, const char* text, int data) {
    int index = (int)SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageA(combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)data);
}

static UiCheckboxState* apd_checkbox_state_from_hwnd(HWND check) {
    if (!check) return nullptr;
    if (check == g_apDialog.enableCheck) return &g_apDialog.enableState;
    if (check == g_apDialog.suppressCheck) return &g_apDialog.suppressState;
    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; ++i) {
        if (check == g_apDialog.focusCheck[i]) {
            return &g_apDialog.focusState[i];
        }
    }
    return nullptr;
}

static bool apd_check_get(HWND check) {
    return ui_checkbox_state_get(apd_checkbox_state_from_hwnd(check));
}

static void apd_check_set(HWND check, bool on) {
    ui_checkbox_state_set(apd_checkbox_state_from_hwnd(check), on);
}

static HWND apd_checkbox_from_id(int id) {
    if (id == APD_ENABLE_CHECK_ID) return g_apDialog.enableCheck;
    if (id == APD_SUPPRESS_CHECK) return g_apDialog.suppressCheck;
    if (id >= APD_RULE_FOCUS_BASE &&
        id < APD_RULE_FOCUS_BASE + AUTO_PROFILE_MAX_RULES) {
        return g_apDialog.focusCheck[id - APD_RULE_FOCUS_BASE];
    }
    return nullptr;
}

static bool apd_checkbox_is_labeled(int id) {
    return id == APD_ENABLE_CHECK_ID || id == APD_SUPPRESS_CHECK;
}

static bool apd_toggle_checkbox(int id) {
    HWND check = apd_checkbox_from_id(id);
    if (!check) return false;
    UiCheckboxState* state = apd_checkbox_state_from_hwnd(check);
    bool previous = ui_checkbox_state_get(state);
    bool next = ui_checkbox_state_toggle(state);
    BOOL redrawn = RedrawWindow(check, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW);
    debug_log("auto-profile dialog checkbox: id=%d previous=%d checked=%d redrawn=%d\n",
        id, previous ? 1 : 0, next ? 1 : 0, redrawn ? 1 : 0);
    return true;
}

static void apd_set_edit_int(HWND edit, int value) {
    char buf[16] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", value);
    if (edit) SetWindowTextA(edit, buf);
}

static int apd_get_edit_int(HWND edit, int fallback) {
    char buf[32] = {};
    if (edit) get_window_text_safe(edit, buf, sizeof(buf));
    int v = fallback;
    if (buf[0] && parse_int_strict(buf, &v)) return v;
    return fallback;
}

// Populate all controls from the current in-memory config + [hotkeys] section.
static void apd_populate() {
    const AutoProfileConfig* cfg = auto_profile_config();
    apd_check_set(g_apDialog.enableCheck, cfg->enabled);
    apd_combo_select(g_apDialog.defaultSlotCombo, cfg->defaultSlot);
    apd_set_edit_int(g_apDialog.debounceEdit, cfg->switchDebounceMs);
    apd_set_edit_int(g_apDialog.cooldownEdit, cfg->minSwitchIntervalMs);
    apd_check_set(g_apDialog.suppressCheck, cfg->suppressWhenWindowOpen);

    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; i++) {
        AutoProfileMatchType type = AUTO_MATCH_NONE;
        bool focus = true;
        int slot = CONFIG_DEFAULT_SLOT;
        char pattern[AUTO_PROFILE_PATTERN_MAX] = {};
        if (i < cfg->ruleCount) {
            type = cfg->rules[i].matchType;
            focus = cfg->rules[i].requireFocus;
            slot = cfg->rules[i].slot;
            StringCchCopyA(pattern, sizeof(pattern), cfg->rules[i].pattern);
        }
        apd_combo_select(g_apDialog.typeCombo[i], type);
        SetWindowTextA(g_apDialog.patternEdit[i], pattern);
        apd_check_set(g_apDialog.focusCheck[i], focus);
        apd_combo_select(g_apDialog.slotCombo[i], slot);
    }

    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        char key[16] = {};
        StringCchPrintfA(key, ARRAY_COUNT(key), "slot%d", s);
        char val[64] = {};
        get_config_string(g_app.configPath, "hotkeys", key, "", val, sizeof(val));
        SetWindowTextA(g_apDialog.hotkeyEdit[s], val);
    }
}

// Gather + validate the controls into a config + a set of hotkey strings.
static bool apd_capture(AutoProfileConfig* out, char hotkeysOut[CONFIG_NUM_SLOTS + 1][64],
                        char* err, size_t errSize) {
    AutoProfileConfig cfg = {};
    auto_profile_config_set_defaults(&cfg);
    cfg.enabled = apd_check_get(g_apDialog.enableCheck);
    cfg.defaultSlot = apd_combo_data(g_apDialog.defaultSlotCombo, CONFIG_DEFAULT_SLOT);
    cfg.switchDebounceMs = apd_get_edit_int(g_apDialog.debounceEdit, AUTO_PROFILE_DEFAULT_DEBOUNCE_MS);
    cfg.minSwitchIntervalMs = apd_get_edit_int(g_apDialog.cooldownEdit, AUTO_PROFILE_DEFAULT_MIN_INTERVAL_MS);
    cfg.suppressWhenWindowOpen = apd_check_get(g_apDialog.suppressCheck);

    int count = 0;
    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; i++) {
        AutoProfileMatchType type = (AutoProfileMatchType)apd_combo_data(g_apDialog.typeCombo[i], AUTO_MATCH_NONE);
        if (type == AUTO_MATCH_NONE) continue;   // skip empty rows (keeps order of the rest)
        char pattern[AUTO_PROFILE_PATTERN_MAX] = {};
        get_window_text_safe(g_apDialog.patternEdit[i], pattern, sizeof(pattern));
        trim_ascii(pattern);
        if ((type == AUTO_MATCH_EXE || type == AUTO_MATCH_TITLE || type == AUTO_MATCH_CLASS) && pattern[0] == 0) {
            set_message(err, errSize, "Rule %d needs a pattern for its match type.", i + 1);
            return false;
        }
        AutoProfileRule* r = &cfg.rules[count];
        r->matchType = type;
        StringCchCopyA(r->pattern, sizeof(r->pattern), pattern);
        r->requireFocus = apd_check_get(g_apDialog.focusCheck[i]);
        r->slot = apd_combo_data(g_apDialog.slotCombo[i], CONFIG_DEFAULT_SLOT);
        count++;
    }
    cfg.ruleCount = count;
    auto_profile_config_normalize(&cfg);

    // Hotkeys: each may be empty; a non-empty binding must parse and carry a
    // modifier (a bare key would hijack it system-wide).
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        hotkeysOut[s][0] = 0;
        char val[64] = {};
        get_window_text_safe(g_apDialog.hotkeyEdit[s], val, sizeof(val));
        trim_ascii(val);
        if (!val[0]) continue;
        HotkeyBinding b = {};
        if (!hotkey_parse(val, &b) || b.mods == 0) {
            set_message(err, errSize, "Hotkey for slot %d must include a modifier, e.g. ctrl+alt+f%d.", s, s);
            return false;
        }
        char canon[64] = {};
        if (hotkey_format(&b, canon, sizeof(canon))) {
            StringCchCopyA(hotkeysOut[s], 64, canon);
        } else {
            StringCchCopyA(hotkeysOut[s], 64, val);
        }
    }

    *out = cfg;
    return true;
}

static bool apd_commit(HWND hwnd) {
    AutoProfileConfig cfg = {};
    char hotkeys[CONFIG_NUM_SLOTS + 1][64] = {};
    char err[256] = {};
    if (!apd_capture(&cfg, hotkeys, err, sizeof(err))) {
        MessageBoxA(hwnd, err, "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!auto_profile_config_save(g_app.configPath, &cfg, hotkeys)) {
        MessageBoxA(hwnd, "Failed to save auto-profile configuration.", "Green Curve", MB_OK | MB_ICONERROR);
        return false;
    }
    auto_profile_reload_config(g_app.hMainWnd);
    return true;
}

static HWND apd_label(HWND hwnd, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        dp(x), dp(y), dp(w), dp(h), hwnd, nullptr, g_app.hInst, nullptr);
}

// Turns a hotkey EDIT into a capture field: focus it and press a key combo; the
// pressed modifiers + key are captured and formatted (no manual typing). The raw
// keystrokes are swallowed so nothing is typed into the edit and the combo never
// leaks to the app. Esc/Backspace/Delete clears it.
static LRESULT CALLBACK apd_hotkey_subclass_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)dwRefData;
    switch (uMsg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            UINT vk = (UINT)wParam;
            if (vk == VK_TAB) break;   // leave Tab for focus navigation
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
                vk == VK_LWIN || vk == VK_RWIN) {
                return 0;              // a lone modifier — wait for the real key
            }
            if (vk == VK_ESCAPE || vk == VK_BACK || vk == VK_DELETE) {
                SetWindowTextA(hWnd, "");
                return 0;
            }
            HotkeyBinding b = {};
            if (GetKeyState(VK_CONTROL) & 0x8000) b.mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000)    b.mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000)   b.mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) b.mods |= MOD_WIN;
            b.vk = vk;
            char text[64] = {};
            // Require a modifier (a bare global hotkey would hijack that key) and
            // a formattable key; otherwise ignore the press and keep the field.
            if (b.mods != 0 && hotkey_format(&b, text, sizeof(text))) {
                SetWindowTextA(hWnd, text);
                SendMessageA(hWnd, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
            }
            return 0;   // swallow so the raw key never types into the edit
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;   // no typed characters / no system beep
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, apd_hotkey_subclass_proc, uIdSubclass);
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static void apd_create_controls(HWND hwnd) {
    apply_system_titlebar_theme(hwnd);
    allow_dark_mode_for_window(hwnd);

    g_apDialog.enableCheck = CreateWindowExA(0, "BUTTON", "Enable auto-profiles",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(16), dp(16), dp(200), dp(20), hwnd, (HMENU)(INT_PTR)APD_ENABLE_CHECK_ID, g_app.hInst, nullptr);

    apd_label(hwnd, "Default profile:", 240, 17, 100, 18);
    g_apDialog.defaultSlotCombo = CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        dp(344), dp(14), dp(110), dp(200), hwnd, (HMENU)(INT_PTR)APD_DEFAULT_SLOT_COMBO, g_app.hInst, nullptr);
    style_combo_control(g_apDialog.defaultSlotCombo);
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        char text[16] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text), "Slot %d", s);
        apd_combo_add(g_apDialog.defaultSlotCombo, text, s);
    }

    apd_label(hwnd, "Debounce (ms):", 16, 48, 100, 18);
    g_apDialog.debounceEdit = CreateWindowExA(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        dp(120), dp(46), dp(70), dp(22), hwnd, (HMENU)(INT_PTR)APD_DEBOUNCE_EDIT, g_app.hInst, nullptr);
    style_input_control(g_apDialog.debounceEdit);

    apd_label(hwnd, "Cooldown (ms):", 210, 48, 100, 18);
    g_apDialog.cooldownEdit = CreateWindowExA(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        dp(314), dp(46), dp(70), dp(22), hwnd, (HMENU)(INT_PTR)APD_COOLDOWN_EDIT, g_app.hInst, nullptr);
    style_input_control(g_apDialog.cooldownEdit);

    g_apDialog.suppressCheck = CreateWindowExA(0, "BUTTON", "Pause while this window is open",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(410), dp(46), dp(280), dp(20), hwnd, (HMENU)(INT_PTR)APD_SUPPRESS_CHECK, g_app.hInst, nullptr);

    apd_label(hwnd, "Rules (evaluated top-down; the first match wins):", 16, 80, 460, 18);
    apd_label(hwnd, "Match", 16, 104, 130, 18);
    apd_label(hwnd, "Pattern (exe name / title / class substring)", 156, 104, 300, 18);
    apd_label(hwnd, "Focus", 556, 104, 50, 18);
    apd_label(hwnd, "Profile", 610, 104, 60, 18);

    for (int i = 0; i < AUTO_PROFILE_MAX_RULES; i++) {
        int y = 126 + i * 30;
        g_apDialog.typeCombo[i] = CreateWindowExA(0, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            dp(16), dp(y), dp(132), dp(220), hwnd, (HMENU)(INT_PTR)(APD_RULE_TYPE_BASE + i), g_app.hInst, nullptr);
        style_combo_control(g_apDialog.typeCombo[i]);
        apd_combo_add(g_apDialog.typeCombo[i], "(disabled)", AUTO_MATCH_NONE);
        apd_combo_add(g_apDialog.typeCombo[i], "Process (.exe)", AUTO_MATCH_EXE);
        apd_combo_add(g_apDialog.typeCombo[i], "Window title", AUTO_MATCH_TITLE);
        apd_combo_add(g_apDialog.typeCombo[i], "Window class", AUTO_MATCH_CLASS);
        apd_combo_add(g_apDialog.typeCombo[i], "Fullscreen app", AUTO_MATCH_FULLSCREEN);

        g_apDialog.patternEdit[i] = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            dp(156), dp(y), dp(392), dp(22), hwnd, (HMENU)(INT_PTR)(APD_RULE_PATTERN_BASE + i), g_app.hInst, nullptr);
        style_input_control(g_apDialog.patternEdit[i]);
        SendMessageA(g_apDialog.patternEdit[i], EM_SETLIMITTEXT, AUTO_PROFILE_PATTERN_MAX - 1, 0);

        g_apDialog.focusCheck[i] = CreateWindowExA(0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            dp(566), dp(y), dp(30), dp(22), hwnd, (HMENU)(INT_PTR)(APD_RULE_FOCUS_BASE + i), g_app.hInst, nullptr);

        g_apDialog.slotCombo[i] = CreateWindowExA(0, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            dp(606), dp(y), dp(84), dp(200), hwnd, (HMENU)(INT_PTR)(APD_RULE_SLOT_BASE + i), g_app.hInst, nullptr);
        style_combo_control(g_apDialog.slotCombo[i]);
        for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
            char text[16] = {};
            StringCchPrintfA(text, ARRAY_COUNT(text), "Slot %d", s);
            apd_combo_add(g_apDialog.slotCombo[i], text, s);
        }
    }

    int hkTop = 126 + AUTO_PROFILE_MAX_RULES * 30 + 12;
    apd_label(hwnd, "Global hotkeys (click a field and press a key combo; press the same hotkey twice to resume auto):",
        16, hkTop, 680, 18);
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        int y = hkTop + 22 + (s - 1) * 28;
        char label[24] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d:", s);
        apd_label(hwnd, label, 16, y + 2, 60, 18);
        g_apDialog.hotkeyEdit[s] = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            dp(80), dp(y), dp(200), dp(22), hwnd, (HMENU)(INT_PTR)(APD_HOTKEY_BASE + s), g_app.hInst, nullptr);
        style_input_control(g_apDialog.hotkeyEdit[s]);
        SendMessageA(g_apDialog.hotkeyEdit[s], EM_SETLIMITTEXT, 40, 0);
        // Capture key combos instead of requiring typed text.
        SetWindowSubclass(g_apDialog.hotkeyEdit[s], apd_hotkey_subclass_proc, (UINT_PTR)s, 0);
        SendMessageW(g_apDialog.hotkeyEdit[s], EM_SETCUEBANNER, TRUE, (LPARAM)L"press a key combo");
    }

    int btnTop = hkTop + 22 + CONFIG_NUM_SLOTS * 28 + 12;
    g_apDialog.saveButton = CreateWindowExA(0, "BUTTON", "Save",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(500), dp(btnTop), dp(88), dp(28), hwnd, (HMENU)(INT_PTR)APD_SAVE_ID, g_app.hInst, nullptr);
    g_apDialog.cancelButton = CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        dp(600), dp(btnTop), dp(88), dp(28), hwnd, (HMENU)(INT_PTR)APD_CANCEL_ID, g_app.hInst, nullptr);

    apply_ui_font_to_children(hwnd);
    apd_populate();
}

static LRESULT CALLBACK AutoProfileDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            apd_create_controls(hwnd);
            return 0;

        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            apply_system_titlebar_theme(hwnd);
            allow_dark_mode_for_window(hwnd);
            break;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON) {
                HWND check = apd_checkbox_from_id((int)dis->CtlID);
                if (check) {
                    draw_themed_checkbox_control(dis, apd_check_get(check),
                        apd_checkbox_is_labeled((int)dis->CtlID));
                    return TRUE;
                }
                draw_themed_button(dis);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int note = HIWORD(wParam);
            if (note == BN_CLICKED && apd_toggle_checkbox(id)) return 0;
            if (id == APD_SAVE_ID && note == BN_CLICKED) {
                if (apd_commit(hwnd)) DestroyWindow(hwnd);
                return 0;
            }
            if (id == APD_CANCEL_ID && note == BN_CLICKED) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(COL_BG);
            FillRect((HDC)wParam, &rc, bg);
            DeleteObject(bg);
            return 1;
        }

        case WM_CTLCOLOREDIT: {
            SetTextColor((HDC)wParam, COL_TEXT);
            SetBkColor((HDC)wParam, COL_INPUT);
            if (!g_apDialog.hEditBrush) g_apDialog.hEditBrush = CreateSolidBrush(COL_INPUT);
            return (LRESULT)g_apDialog.hEditBrush;
        }

        case WM_CTLCOLORBTN: {
            SetTextColor((HDC)wParam, COL_TEXT);
            SetBkColor((HDC)wParam, COL_BG);
            if (!g_apDialog.hBtnBrush) g_apDialog.hBtnBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)g_apDialog.hBtnBrush;
        }

        case WM_CTLCOLORLISTBOX: {
            SetTextColor((HDC)wParam, COL_TEXT);
            SetBkColor((HDC)wParam, COL_INPUT);
            if (!g_apDialog.hListBrush) {
                g_apDialog.hListBrush = CreateSolidBrush(COL_INPUT);
            }
            return (LRESULT)g_apDialog.hListBrush;
        }

        case WM_CTLCOLORSTATIC: {
            SetTextColor((HDC)wParam, COL_LABEL);
            SetBkColor((HDC)wParam, COL_BG);
            if (!g_apDialog.hStaticBrush) g_apDialog.hStaticBrush = CreateSolidBrush(COL_BG);
            return (LRESULT)g_apDialog.hStaticBrush;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_apDialog.hEditBrush) { DeleteObject(g_apDialog.hEditBrush); g_apDialog.hEditBrush = nullptr; }
            if (g_apDialog.hStaticBrush) { DeleteObject(g_apDialog.hStaticBrush); g_apDialog.hStaticBrush = nullptr; }
            if (g_apDialog.hBtnBrush) { DeleteObject(g_apDialog.hBtnBrush); g_apDialog.hBtnBrush = nullptr; }
            if (g_apDialog.hListBrush) { DeleteObject(g_apDialog.hListBrush); g_apDialog.hListBrush = nullptr; }
            if (g_app.hMainWnd) {
                EnableWindow(g_app.hMainWnd, TRUE);
                SetForegroundWindow(g_app.hMainWnd);
            }
            memset(&g_apDialog, 0, sizeof(g_apDialog));
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void auto_profile_open_config_dialog(HWND parent) {
    if (g_apDialog.hwnd) {
        ShowWindow(g_apDialog.hwnd, SW_SHOW);
        SetForegroundWindow(g_apDialog.hwnd);
        return;
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AutoProfileDialogProc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = "GreenCurveAutoProfileDialog";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = (HICON)SendMessageA(parent ? parent : g_app.hMainWnd, WM_GETICON, ICON_SMALL, 0);
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing)) {
        RegisterClassExA(&wc);
    }

    int clientW = 712;
    int clientH = 126 + AUTO_PROFILE_MAX_RULES * 30 + 12 + 22 + CONFIG_NUM_SLOTS * 28 + 12 + 44;
    SIZE size = adjusted_window_size_for_client(dp(clientW), dp(clientH),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME);

    HWND owner = parent ? parent : g_app.hMainWnd;
    RECT ownerRect = {};
    if (owner) GetWindowRect(owner, &ownerRect);
    RECT work = owner
        ? main_window_work_area(owner)
        : main_window_work_area_for_rect(nullptr);
    MainLayoutRect anchor = main_layout_rect_from_win32(owner ? ownerRect : work);
    RECT centered = main_layout_rect_to_win32(
        main_layout_center_rect(anchor, size.cx, size.cy));
    RECT target = clamp_window_rect_to_work_area(centered, work);
    debug_log("auto-profile dialog placement: owner=%ld,%ld-%ld,%ld work=%ld,%ld-%ld,%ld target=%ld,%ld %ldx%ld\n",
        ownerRect.left, ownerRect.top, ownerRect.right, ownerRect.bottom,
        work.left, work.top, work.right, work.bottom,
        target.left, target.top, target.right - target.left,
        target.bottom - target.top);

    g_apDialog.hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName,
        "Configure Auto-Profiles",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        target.left, target.top, target.right - target.left,
        target.bottom - target.top, owner, nullptr, g_app.hInst, nullptr);
    if (!g_apDialog.hwnd) return;

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(g_apDialog.hwnd, SW_SHOW);
    UpdateWindow(g_apDialog.hwnd);
}
