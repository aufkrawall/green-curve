// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

static inline bool profile_should_strip_legacy_unlocked_curve(
    bool lockCiWasExplicit, int lockCi, bool explicitVfPointsFormat) {
    return lockCiWasExplicit && lockCi < 0 && !explicitVfPointsFormat;
}
