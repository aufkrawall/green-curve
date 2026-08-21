// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Duplicate-launch activation.  Included by the Windows GUI amalgamation just
// ahead of the tray/runtime shard that calls it.

static bool activate_existing_instance_window() {
    HANDLE ready = OpenEventA(SYNCHRONIZE, FALSE,
                              APP_SINGLE_INSTANCE_READY_NAME);
    if (ready) {
        // This is a failure-containment bound, not a startup race: the first
        // process signals an explicit event immediately after HWND creation.
        // The bound prevents a duplicate from holding the named event forever
        // if its handle is the only remaining reference and creation failed.
        (void)WaitForSingleObject(ready, 10000);
        CloseHandle(ready);
    }

    HWND existing = FindWindowA(APP_CLASS_NAME, nullptr);
    if (!existing || !IsWindow(existing)) return false;
    HWND target = GetLastActivePopup(existing);
    if (!target || !IsWindow(target)) target = existing;
    if (target != existing) {
        ShowWindow(target, SW_SHOW);
        ShowWindow(target, SW_RESTORE);
        BringWindowToTop(target);
        SetForegroundWindow(target);
    } else {
        // Route main-window activation back through its own GUI thread so
        // hidden-to-tray state, reconnect sync, and GDI retirement remain one
        // transaction.
        PostMessageA(existing, APP_WM_ACTIVATE_EXISTING_INSTANCE, 0, 0);
    }
    return true;
}
