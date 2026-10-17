// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The window thread's announcement of a new automatic-restore lockout
// (auto_apply_failure_notice_policy.h), split out of gui_service_state.cpp.
// Called after each READY adoption and its startup/status projection. Its own
// observed reason is independent of snapshots adopted before window creation.
#ifndef GREEN_CURVE_GUI_AUTO_RESTORE_NOTICE_H
#define GREEN_CURVE_GUI_AUTO_RESTORE_NOTICE_H

#include "auto_apply_failure_notice_policy.h"

// A balloon composed while no tray icon existed yet (the GUI is still starting
// at logon, which is exactly when a logon apply fails) waits here for the icon
// instead of being spent on nothing.  Window thread only.
static bool g_guiAutoApplyBalloonPending = false;
static char g_guiAutoApplyBalloonTitle[64] = {};
static char g_guiAutoApplyBalloonBody[256] = {};
static gc_u32 g_guiNoticeObservedLockout = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;

static void gui_auto_restore_notice_flush_balloon() {
    if (!g_guiAutoApplyBalloonPending || !g_app.trayIconAdded) return;
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    // A warning, but silent: this fires at logon, usually before a game starts.
    nid.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    StringCchCopyA(nid.szInfoTitle, ARRAY_COUNT(nid.szInfoTitle),
                   g_guiAutoApplyBalloonTitle);
    StringCchCopyA(nid.szInfo, ARRAY_COUNT(nid.szInfo), g_guiAutoApplyBalloonBody);
    if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
        g_guiAutoApplyBalloonPending = false;
        debug_log("GUI: automatic apply failure tray notice shown\n");
    } else {
        // Not consumed: the shell can refuse while the notification area is
        // busy, and the next adoption is a free retry.
        debug_log_on_change("GUI: automatic apply failure tray notice refused by the shell (error %lu); will retry\n",
                            GetLastError());
    }
}

static void gui_announce_auto_restore_lockout() {
    // Startup can already have adopted a failed-apply snapshot before there
    // is a window. Snapshot state is not evidence that its notice was shown.
    const gc_u32 previous = g_guiNoticeObservedLockout;
    GcAutoRestoreNotice notice = gc_auto_restore_notice_observe(
        &g_guiNoticeObservedLockout, (gc_u32)g_app.serviceAutoRestoreLockoutReason);
    if (notice == GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE) {
        // The one lockout reason that names hardware left in an unknown state:
        // the service stopped unexpectedly and could not return the GPU to
        // stock.  The user's Reset clears it.
        debug_log("GUI: service reports an incomplete handback after an unexpected stop\n");
        set_profile_status_text("Service stopped unexpectedly; GPU may not be at stock. Press Reset.");
    } else if (notice == GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED) {
        const ServiceAutoApplyFailure* failure = &g_app.serviceAutoApplyFailure;
        char status[256] = {};
        if (gc_auto_apply_failure_compose(failure, status, sizeof(status),
                g_guiAutoApplyBalloonTitle, sizeof(g_guiAutoApplyBalloonTitle),
                g_guiAutoApplyBalloonBody, sizeof(g_guiAutoApplyBalloonBody))) {
            debug_log("GUI: automatic apply failure notice (previous=%u trigger=%u coreRecovered=%d tray=%d): %s\n",
                (unsigned int)previous, (unsigned int)failure->trigger, failure->coreRecovered ? 1 : 0,
                g_app.trayIconAdded ? 1 : 0, status);
            set_profile_status_text("%s", status);
            // A failure this service instance saw is news every time.  The
            // causeless "still paused" form gets at most one balloon per GUI
            // process, even if automation is cleared and later paused again;
            // each new appearance still gets its status line.
            static bool s_pausedBalloonShown = false;
            bool fresh = failure->trigger != SERVICE_LIFECYCLE_TRIGGER_NONE;
            if (fresh || !s_pausedBalloonShown) {
                s_pausedBalloonShown = s_pausedBalloonShown || !fresh;
                g_guiAutoApplyBalloonPending = true;
            }
        }
    } else if (g_app.serviceAutoRestoreLockoutReason ==
               SERVICE_AUTO_RESTORE_LOCKOUT_NONE) {
        // Cleared (an explicit Apply succeeded): a balloon still waiting for
        // the tray icon is no longer true.
        g_guiAutoApplyBalloonPending = false;
    }
    gui_auto_restore_notice_flush_balloon();
}

#endif // GREEN_CURVE_GUI_AUTO_RESTORE_NOTICE_H
