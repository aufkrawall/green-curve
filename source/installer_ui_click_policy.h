// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which WM_COMMAND notifications the setup wizard treats as a user click.
//
// The wizard's action buttons are BS_OWNERDRAW.  A fast double-click on such
// a button is delivered by the real button control as BN_CLICKED (the first
// click) followed by BN_DBLCLK (the second click) -- NOT as a second
// BN_CLICKED, and the trailing release adds nothing (pinned by the native
// owner-draw fixture in tests/regression_main.cpp).  A handler that filters
// to BN_CLICKED alone therefore swallows the second click of every fast
// double-click, which makes rapid navigation through the installer pages
// feel like the button ignored the user.
//
// Action buttons accept both notifications.  The owner-drawn checkboxes stay
// one-toggle-per-gesture: accepting BN_DBLCLK there would toggle a checkbox a
// second time and silently undo the very click the user just made.

#ifndef GREEN_CURVE_INSTALLER_UI_CLICK_POLICY_H
#define GREEN_CURVE_INSTALLER_UI_CLICK_POLICY_H

// Host-neutral spellings of the Win32 button notification codes.  installer_ui.cpp
// static_asserts them against winuser.h so they cannot drift from the real
// values while this header stays compilable in the either-host harness.
enum {
    GC_WIZARD_NOTIFY_CLICKED = 0,  // BN_CLICKED
    GC_WIZARD_NOTIFY_DBLCLK = 5,   // BN_DBLCLK
};

static inline bool gc_wizard_notification_is_click(unsigned int notification,
                                                   bool isCheckbox) {
    if (notification == GC_WIZARD_NOTIFY_CLICKED) return true;
    // BN_DBLCLK is the OS-classified second half of a fast double-click.  It
    // is a real click on an action button; on a checkbox it would toggle the
    // control a second time and undo the first click.
    return notification == GC_WIZARD_NOTIFY_DBLCLK && !isCheckbox;
}

#endif  // GREEN_CURVE_INSTALLER_UI_CLICK_POLICY_H
