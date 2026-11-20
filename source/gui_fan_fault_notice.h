// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The window thread's announcement of a fan the service handed back (protocol
// v39 ServiceFanFault, fan_fault_notice_policy.h).  Called after each READY
// adoption, after the automatic-restore notice, so the fan -- the one domain
// that is unsafe without a controller -- has the last word on the status line.
#ifndef GREEN_CURVE_GUI_FAN_FAULT_NOTICE_H
#define GREEN_CURVE_GUI_FAN_FAULT_NOTICE_H

#include "gui_auto_restore_notice.h"
#include "fan_fault_notice_policy.h"

// Window thread only.  A notice composed before the tray icon exists waits for
// it, like the automatic-restore balloon.
static GcFanFaultObserver g_guiFanFaultObserver = {};
static bool g_guiFanFaultBalloonPending = false;
static char g_guiFanFaultBalloonTitle[64] = {};
static char g_guiFanFaultBalloonBody[256] = {};

static void gui_fan_fault_flush_balloon() {
    if (!g_guiFanFaultBalloonPending || !g_app.trayIconAdded) return;
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    // Audible on purpose: unlike a paused automation, this is about cooling.
    nid.dwInfoFlags = NIIF_WARNING;
    StringCchCopyA(nid.szInfoTitle, ARRAY_COUNT(nid.szInfoTitle), g_guiFanFaultBalloonTitle);
    StringCchCopyA(nid.szInfo, ARRAY_COUNT(nid.szInfo), g_guiFanFaultBalloonBody);
    if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
        g_guiFanFaultBalloonPending = false;
        debug_log("GUI: fan fault tray notice shown\n");
    } else {
        debug_log_on_change("GUI: fan fault tray notice refused by the shell (error %lu); will retry\n",
                            GetLastError());
    }
}

static void gui_announce_fan_fault() {
    const ServiceFanFault* fault = &g_app.serviceFanFault;
    if (gc_fan_fault_observe(&g_guiFanFaultObserver, fault)) {
        char status[256] = {};
        if (gc_fan_fault_compose(fault, status, sizeof(status),
                g_guiFanFaultBalloonTitle, sizeof(g_guiFanFaultBalloonTitle),
                g_guiFanFaultBalloonBody, sizeof(g_guiFanFaultBalloonBody))) {
            debug_log("GUI: fan fault notice (kind=%s serial=%u fan=%u pct=%u handback=%s tray=%d): %s\n",
                gc_fan_fault_kind_name(fault->kind), (unsigned int)fault->serial,
                (unsigned int)fault->fanIndex, (unsigned int)fault->commandedPercent,
                gc_fan_fault_handback_name(fault->handback),
                g_app.trayIconAdded ? 1 : 0, status);
            set_profile_status_text("%s", status);
            g_guiFanFaultBalloonPending = true;
        }
    } else if (fault->kind == SERVICE_FAN_FAULT_NONE) {
        // Cleared by an Apply or Reset: a balloon still waiting is no longer true.
        g_guiFanFaultBalloonPending = false;
    }
    gui_fan_fault_flush_balloon();
}

#endif // GREEN_CURVE_GUI_FAN_FAULT_NOTICE_H
