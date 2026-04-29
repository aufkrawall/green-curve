// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "string_table.h"

struct StringTable {
    const char* entries[static_cast<int>(StringId::Count)];
};

static const StringTable g_english = {
    {
        /* None                */ "",
        /* AppName             */ "Green Curve",
        /* AppVersion          */ APP_VERSION,
        /* ServiceNotInstalled */ "Background service is not installed. Live controls disabled.",
        /* ServiceInstalled    */ "Background service installed",
        /* ServiceNotResponding*/ "Background service is installed but not responding. Live controls are disabled.",
        /* ServiceStopped      */ "Background service installed but stopped. Live controls are disabled.",
        /* LiveControlsDisabled*/ "Live GPU control is unavailable until the service is installed again.",
        /* ApplyFailed         */ "Apply failed",
        /* ApplySuccess        */ "Apply succeeded",
        /* ProfileSaved        */ "Profile saved",
        /* ProfileLoaded       */ "Profile loaded",
        /* ConfigSaved         */ "Configuration saved",
        /* ConfigLoadError     */ "Failed to load configuration",
        /* GpuNotDetected      */ "No NVIDIA GPU detected",
        /* UnsupportedGpu      */ "Unsupported GPU family",
        /* ProbeSaved          */ "Probe report saved",
        /* SafetyWarning       */ "Manual GPU tuning can cause instability or damage. Use at your own risk.",
        /* FanModeAuto         */ "Auto",
        /* FanModeFixed        */ "Fixed",
        /* FanModeCurve        */ "Curve",
    }
};

static const StringTable* g_activeTable = &g_english;

const char* get_string(StringId id) {
    int index = static_cast<int>(id);
    if (index <= 0 || index >= static_cast<int>(StringId::Count)) return "";
    const char* text = g_activeTable->entries[index];
    return text ? text : "";
}
