// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Input transaction and transition handling for the owner-drawn VF lock
// controls. This is a GUI amalgamation shard included after apply_lock() and
// unlock_all() are defined.

static LockUiStateStamp current_lock_ui_state_stamp() {
    LockUiStateStamp stamp = {};
    stamp.lockedVi = g_app.lockedVi;
    stamp.lockedCi = g_app.lockedCi;
    stamp.lockedFreq = g_app.lockedFreq;
    stamp.lockMode = g_app.lockMode;
    return stamp;
}

static void arm_lock_input_gesture(HWND hWnd, UINT sourceMessage) {
    int vi = GetDlgCtrlID(hWnd) - LOCK_BASE_ID;
    g_lockInputGesture.armed = vi >= 0 && vi < g_app.numVisible;
    g_lockInputGesture.consumed = false;
    g_lockInputGesture.suppressNextMouseRelease = false;
    g_lockInputGesture.vi = vi;
    g_lockInputGesture.suppressedVi = -1;
    g_lockInputGesture.sourceMessage = sourceMessage;
    g_lockInputGesture.messageTime = (DWORD)GetMessageTime();
    g_lockInputGesture.pressState = current_lock_ui_state_stamp();
    debug_log("lock input begin: vi=%d source=0x%04x msgTime=%lu armed=%d state=(vi=%d ci=%d mhz=%u mode=%s)\n",
              vi, (unsigned)sourceMessage, (unsigned long)g_lockInputGesture.messageTime,
              g_lockInputGesture.armed ? 1 : 0,
              g_lockInputGesture.pressState.lockedVi,
              g_lockInputGesture.pressState.lockedCi,
              g_lockInputGesture.pressState.lockedFreq,
              lock_mode_name(g_lockInputGesture.pressState.lockMode));
}

static void finish_lock_input_gesture(HWND hWnd, UINT sourceMessage, const char* outcome) {
    int vi = GetDlgCtrlID(hWnd) - LOCK_BASE_ID;
    debug_log("lock input end: vi=%d source=0x%04x msgTime=%lu outcome=%s armed=%d consumed=%d\n",
              vi, (unsigned)sourceMessage, (unsigned long)(DWORD)GetMessageTime(),
              outcome ? outcome : "unspecified",
              g_lockInputGesture.armed ? 1 : 0,
              g_lockInputGesture.consumed ? 1 : 0);
    g_lockInputGesture = {};
}

static LRESULT CALLBACK lock_checkbox_subclass_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)dwRefData;
    if (uMsg == WM_LBUTTONDOWN) {
        arm_lock_input_gesture(hWnd, uMsg);
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_SPACE && (lParam & (1LL << 30)) == 0) {
        arm_lock_input_gesture(hWnd, uMsg);
    }
    if (uMsg == WM_LBUTTONDBLCLK) {
        int vi = GetDlgCtrlID(hWnd) - LOCK_BASE_ID;
        debug_log("lock checkbox: ignored double-click message for vi=%d msgTime=%lu state=(vi=%d ci=%d mhz=%u mode=%s)\n",
                  vi, (unsigned long)(DWORD)GetMessageTime(),
                  g_app.lockedVi, g_app.lockedCi, g_app.lockedFreq, lock_mode_name(g_app.lockMode));
        finish_lock_input_gesture(hWnd, uMsg, "double-click suppressed");
        // A native double-click is DOWN, UP, DBLCLK, UP. Since the DBLCLK was
        // not passed to the BUTTON procedure, suppress its paired release too.
        g_lockInputGesture.suppressNextMouseRelease = true;
        g_lockInputGesture.suppressedVi = vi;
        return 0;
    }
    if (uMsg == WM_LBUTTONUP && g_lockInputGesture.suppressNextMouseRelease &&
        g_lockInputGesture.suppressedVi == GetDlgCtrlID(hWnd) - LOCK_BASE_ID) {
        finish_lock_input_gesture(hWnd, uMsg, "paired double-click release suppressed");
        return 0;
    }
    if (uMsg == WM_LBUTTONUP || (uMsg == WM_KEYUP && wParam == VK_SPACE)) {
        // The BUTTON procedure synchronously sends BN_CLICKED during release.
        // Keep the press stamp armed until WndProc has consumed it.
        LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        finish_lock_input_gesture(hWnd, uMsg, "release dispatched");
        return result;
    }
    if (uMsg == WM_NCDESTROY) {
        if (g_lockInputGesture.armed &&
            g_lockInputGesture.vi == GetDlgCtrlID(hWnd) - LOCK_BASE_ID) {
            finish_lock_input_gesture(hWnd, uMsg, "control destroyed");
        }
        RemoveWindowSubclass(hWnd, lock_checkbox_subclass_proc, uIdSubclass);
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static void activate_lock_checkbox_once(int vi) {
    bool samePoint = vi == g_app.lockedVi;
    LockMode previousMode = samePoint ? g_app.lockMode : LOCK_MODE_NONE;
    LockMode nextMode = lock_mode_after_activation(samePoint, previousMode);
    debug_log("lock checkbox transition: vi=%d samePoint=%d prevMode=%s nextMode=%s prevLockedVi=%d\n",
              vi, samePoint ? 1 : 0, lock_mode_name(previousMode),
              lock_mode_name(nextMode), g_app.lockedVi);

    if (!samePoint) {
        apply_lock(vi, LOCK_MODE_FLATTEN);
        return;
    }
    if (nextMode == LOCK_MODE_HARD) {
        g_app.lockMode = LOCK_MODE_HARD;
        SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_CHECKED, 0);
        InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
        if (g_app.lockedCi >= 0) {
            record_ui_action("hard lock point %d @ %u MHz (pinned)",
                             g_app.lockedCi, g_app.lockedFreq);
        }
        set_gui_state_dirty(true);
        return;
    }

    SendMessageA(g_app.hLocks[vi], BM_SETCHECK, BST_UNCHECKED, 0);
    InvalidateRect(g_app.hLocks[vi], nullptr, FALSE);
    if (g_app.lockedCi >= 0) {
        record_ui_action("unlock point %d (was %s)",
                         g_app.lockedCi, lock_mode_name(g_app.lockMode));
    }
    unlock_all();
    set_gui_state_dirty(true);
}
