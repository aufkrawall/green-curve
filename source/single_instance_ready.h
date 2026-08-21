// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// One explicit edge tells a duplicate launch that first-window creation has
// completed (successfully or not).  This replaces polling for a window class.
#ifndef GREEN_CURVE_SINGLE_INSTANCE_READY_H
#define GREEN_CURVE_SINGLE_INSTANCE_READY_H

static void signal_single_instance_window_ready() {
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                              APP_SINGLE_INSTANCE_READY_NAME);
    if (!ready) return;
    SetEvent(ready);
    CloseHandle(ready);
}

#endif
