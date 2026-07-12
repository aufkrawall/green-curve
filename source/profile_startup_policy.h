// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

enum StartupEditorSource {
    STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT = 0,
    STARTUP_EDITOR_SOURCE_APP_LAUNCH_PROFILE = 1,
    STARTUP_EDITOR_SOURCE_LOGON_SERVICE = 2,
};

// A selected profile slot is only a storage/UI selection. It must never decide
// which values the editor presents at startup; loading saved intent requires an
// explicit Load action or an enabled app-launch profile.
static inline StartupEditorSource startup_editor_source(bool launchedFromLogon,
                                                         int appLaunchSlot,
                                                         int maxProfileSlots) {
    if (launchedFromLogon) return STARTUP_EDITOR_SOURCE_LOGON_SERVICE;
    if (appLaunchSlot >= 1 && appLaunchSlot <= maxProfileSlots) {
        return STARTUP_EDITOR_SOURCE_APP_LAUNCH_PROFILE;
    }
    return STARTUP_EDITOR_SOURCE_LIVE_SNAPSHOT;
}
