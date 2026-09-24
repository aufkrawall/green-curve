// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_UPDATE_INSTALL_FAN_POLICY_H
#define GREEN_CURVE_UPDATE_INSTALL_FAN_POLICY_H

// What happens to a Green Curve-driven fan while an in-app update installs.
//
// The install reservation blocks every GPU write, the fan runtime's pulse
// included, until setup stops the service.  That is normally seconds, but the
// reservation may live for the whole installer timeout (ten minutes) when
// setup stalls -- an antivirus scan of a freshly downloaded unsigned installer
// is enough.  A blocked pulse does not stop the fan: it freezes it at the last
// manual duty while the temperature is no longer being tracked.
//
// So the reservation hands a manual fan runtime to driver auto first, and the
// paths that release the reservation WITHOUT setup stopping the service put the
// runtime back.  A successful install needs no restore: setup stops the
// service, whose graceful shutdown returns the fan to the driver anyway.
//
// Windows-free, so the regression harness exercises it on both hosts.

enum UpdateInstallFanRuntime {
    UPDATE_INSTALL_FAN_RUNTIME_NONE = 0,
    UPDATE_INSTALL_FAN_RUNTIME_CURVE = 1,
    UPDATE_INSTALL_FAN_RUNTIME_FIXED = 2,
};

struct UpdateInstallFanHandback {
    UpdateInstallFanRuntime runtime;
    int fixedPercent;
};

static inline UpdateInstallFanHandback update_install_fan_handback_capture(
    bool curveRuntimeActive, bool fixedRuntimeActive, int fixedPercent) {
    UpdateInstallFanHandback handback = {};
    // The two runtimes are exclusive; if both flags were ever set, the curve
    // is the one that tracks temperature and is the safer one to bring back.
    if (curveRuntimeActive) {
        handback.runtime = UPDATE_INSTALL_FAN_RUNTIME_CURVE;
    } else if (fixedRuntimeActive) {
        handback.runtime = UPDATE_INSTALL_FAN_RUNTIME_FIXED;
        handback.fixedPercent = fixedPercent < 0 ? 0 : (fixedPercent > 100 ? 100 : fixedPercent);
    }
    return handback;
}

static inline bool update_install_fan_handback_needed(
    const UpdateInstallFanHandback* handback) {
    return handback && handback->runtime != UPDATE_INSTALL_FAN_RUNTIME_NONE;
}

// Which runtime to restart once the reservation is released on a failure path.
// Nothing is restarted while the reservation is still held (setup may be
// replacing files), and a runtime something else already restarted is not
// started a second time.
static inline UpdateInstallFanRuntime update_install_fan_restore_plan(
    const UpdateInstallFanHandback* handback, bool reservationReleased,
    bool runtimeAlreadyActive) {
    if (!handback || !reservationReleased || runtimeAlreadyActive)
        return UPDATE_INSTALL_FAN_RUNTIME_NONE;
    return handback->runtime;
}

static inline const char* update_install_fan_runtime_name(UpdateInstallFanRuntime runtime) {
    switch (runtime) {
        case UPDATE_INSTALL_FAN_RUNTIME_NONE: return "none";
        case UPDATE_INSTALL_FAN_RUNTIME_CURVE: return "curve";
        case UPDATE_INSTALL_FAN_RUNTIME_FIXED: return "fixed";
    }
    return "unknown";
}

#endif // GREEN_CURVE_UPDATE_INSTALL_FAN_POLICY_H
