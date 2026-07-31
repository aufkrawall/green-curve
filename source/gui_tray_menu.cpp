// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The tray context menu plus the throwaway window it is tracked against.
//
// Right-clicking the tray icon used to raise the main window: the menu was
// tracked against the main window, and the SetForegroundWindow() the shell
// requires before TrackPopupMenu() also moves its target to the top of the
// z order.  With Green Curve open behind another window, one tray click threw
// it in front of whatever the user was working in until the menu closed.
//
// The menu is now tracked against a zero-sized, never-shown tool window that
// exists only to hold the foreground for the popup's modal loop, so the main
// window's z order and visibility are not touched at all.  See
// gui_tray_callback_policy.h for the neutrality rule this file enforces.
//
// Split out of main_fan_runtime.cpp, which is at its size ratchet.

#define TRAY_MENU_OWNER_CLASS_NAME "GreenCurveTrayMenuOwner"

static HWND g_trayMenuOwnerWnd = nullptr;

static LRESULT CALLBACK tray_menu_owner_wndproc(HWND hwnd, UINT msg,
                                                WPARAM wParam, LPARAM lParam) {
    // The popup is tracked with TPM_RETURNCMD, so this owner normally never
    // sees a menu command at all.  Forward it anyway: if that flag is ever
    // dropped the picks keep working instead of being silently swallowed by a
    // window with no menu handler.
    if (msg == WM_COMMAND && g_app.hMainWnd && hwnd == g_trayMenuOwnerWnd) {
        debug_log("tray menu: owner forwarded command %u to the main window\n",
                  (unsigned)LOWORD(wParam));
        PostMessageA(g_app.hMainWnd, WM_COMMAND, wParam, lParam);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void destroy_tray_menu_owner_window() {
    if (!g_trayMenuOwnerWnd) return;
    HWND owner = g_trayMenuOwnerWnd;
    g_trayMenuOwnerWnd = nullptr;
    DestroyWindow(owner);
    debug_log("tray menu: owner window destroyed hwnd=%p\n", (void*)owner);
}

// Create (once) the neutral foreground owner.  Returns nullptr rather than a
// window that would be visible to the user -- the caller decides what to do
// with that, and it is never allowed to silently become the main window here.
static HWND ensure_tray_menu_owner_window() {
    if (g_trayMenuOwnerWnd && IsWindow(g_trayMenuOwnerWnd)) {
        return g_trayMenuOwnerWnd;
    }
    g_trayMenuOwnerWnd = nullptr;

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_menu_owner_wndproc;
    wc.hInstance = g_app.hInst;
    wc.lpszClassName = TRAY_MENU_OWNER_CLASS_NAME;
    WNDCLASSEXA existing = {};
    if (!GetClassInfoExA(g_app.hInst, wc.lpszClassName, &existing) &&
        !RegisterClassExA(&wc)) {
        debug_log("tray menu: owner class registration failed lastError=%lu\n",
                  GetLastError());
        return nullptr;
    }

    // Zero-sized, unowned, never shown.  WS_EX_TOOLWINDOW keeps it out of
    // Alt-Tab; WS_EX_NOACTIVATE is deliberately absent because being activated
    // is this window's only purpose.  A distinct class name keeps the
    // single-instance FindWindowA(APP_CLASS_NAME) lookup from ever landing on
    // it instead of the real main window.
    HWND owner = CreateWindowExA(
        WS_EX_TOOLWINDOW, TRAY_MENU_OWNER_CLASS_NAME, "",
        WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, g_app.hInst, nullptr);
    if (!owner) {
        debug_log("tray menu: owner window creation failed lastError=%lu\n",
                  GetLastError());
        return nullptr;
    }

    LONG_PTR style = GetWindowLongPtrA(owner, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrA(owner, GWL_EXSTYLE);
    bool neutral = gui_tray_menu_owner_is_neutral(
        owner == g_app.hMainWnd,
        (style & WS_VISIBLE) != 0,
        (exStyle & WS_EX_APPWINDOW) != 0,
        (exStyle & WS_EX_TOOLWINDOW) != 0,
        (exStyle & WS_EX_NOACTIVATE) != 0);
    debug_log("tray menu: owner window created hwnd=%p style=0x%08lX exStyle=0x%08lX neutral=%d\n",
              (void*)owner, (unsigned long)style, (unsigned long)exStyle,
              neutral ? 1 : 0);
    if (!neutral) {
        // An owner the user can see is the exact defect this window exists to
        // prevent, so it is not kept.
        DestroyWindow(owner);
        return nullptr;
    }
    g_trayMenuOwnerWnd = owner;
    return g_trayMenuOwnerWnd;
}

static void show_tray_menu(HWND hwnd) {
    if (!hwnd) return;

    HWND owner = ensure_tray_menu_owner_window();
    bool neutralOwner = owner != nullptr;
    if (!neutralOwner) {
        // With no foreground window at all the popup outlives the click that
        // should dismiss it, and for a tray-resident instance this menu is the
        // only way to quit.  Take the old raise over a stuck menu, loudly.
        owner = hwnd;
        debug_log("tray menu: no neutral owner available; tracking against the main window (it will be raised)\n");
    }
    // The shell paints a menu from its owner's dark-mode opt-in, and
    // refresh_menu_theme_cache() flushes the cached menu theme, so the
    // throwaway owner has to declare itself first -- the same order the main
    // window uses on WM_THEMECHANGED.  Repeating it per popup also picks up an
    // OS theme change made since the owner was created.
    allow_dark_mode_for_window(owner);
    refresh_menu_theme_cache();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, TRAY_MENU_SHOW_ID,
        IsWindowVisible(hwnd) ? "Show Window" : "Open Green Curve");
    HMENU profiles = build_auto_profile_menu();
    if (profiles) AppendMenuA(menu, MF_POPUP, (UINT_PTR)profiles, "Profiles");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, TRAY_MENU_EXIT_ID, "Exit");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    int cmd = (int)TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN |
        TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, owner, nullptr);
    // Second half of the shell's own workaround: without a message posted to
    // the owner after the popup closes, the click that dismissed it can be
    // swallowed and the menu reappear.
    PostMessageA(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);   // destroys the attached Profiles submenu too

    debug_log("tray menu: closed command=%d neutralOwner=%d mainVisible=%d mainRaised=%d\n",
              cmd, neutralOwner ? 1 : 0, IsWindowVisible(hwnd) ? 1 : 0,
              neutralOwner ? 0 : 1);
    if (cmd > 0) {
        PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
    }
}
