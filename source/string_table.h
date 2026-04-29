// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Minimal string-table scaffolding for future i18n.
// To add a new language, duplicate g_english table, translate, and switch
// g_activeTable at runtime (e.g. via a config key or CLI flag).

enum class StringId {
    None = 0,
    AppName,
    AppVersion,
    ServiceNotInstalled,
    ServiceInstalled,
    ServiceNotResponding,
    ServiceStopped,
    LiveControlsDisabled,
    ApplyFailed,
    ApplySuccess,
    ProfileSaved,
    ProfileLoaded,
    ConfigSaved,
    ConfigLoadError,
    GpuNotDetected,
    UnsupportedGpu,
    ProbeSaved,
    SafetyWarning,
    FanModeAuto,
    FanModeFixed,
    FanModeCurve,
    Count
};

const char* get_string(StringId id);

// Convenience macro to keep call sites readable.
#define S(id) get_string(StringId::id)
