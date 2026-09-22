// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

// The setup helper must distinguish a successful read that found no active
// intent from a failed read. Older clients return 1 for both; setup treats that
// answer as uncertain and tries the payload client before reporting a warning.
enum { GC_SETTINGS_TRANSFER_NO_ACTIVE_EXIT_CODE = 4 };

enum GcSettingsCaptureResult {
    GC_SETTINGS_CAPTURE_NOT_REQUESTED = 0,
    GC_SETTINGS_CAPTURE_SAVED,
    GC_SETTINGS_CAPTURE_NONE_ACTIVE,
    GC_SETTINGS_CAPTURE_FAILED,
};

static inline GcSettingsCaptureResult gc_settings_capture_attempt_result(
    bool ran, unsigned long exitCode, bool snapshotReadable) {
    if (ran && exitCode == 0 && snapshotReadable) return GC_SETTINGS_CAPTURE_SAVED;
    if (ran && exitCode == GC_SETTINGS_TRANSFER_NO_ACTIVE_EXIT_CODE &&
        !snapshotReadable) return GC_SETTINGS_CAPTURE_NONE_ACTIVE;
    return GC_SETTINGS_CAPTURE_FAILED;
}
