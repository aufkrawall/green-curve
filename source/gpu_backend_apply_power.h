// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_POWER_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_POWER_H

// Included after backend declarations; power previews never decide a ratio write.
template<class AppendFailure>
static void apply_power_settings(const DesiredSettings* desired, bool powerTargetWrittenByReset,
    unsigned int expectedDefaultMw, int& successCount, int& failCount,
    bool& powerChanged, bool& partialApplyRisk, AppendFailure append_failure) {
    if (desired->hasPowerLimit && desired->powerLimitRatio) {
        powerChanged = true;
        if (nvapi_set_power_limit_ratio(desired_power_ratio(*desired),
                g_app.powerLimitWriteUncertain, expectedDefaultMw)) ++successCount;
        else {
            ++failCount; partialApplyRisk = true;
            append_failure("Exact power target was not accepted or its destination changed");
        }
    } else if (desired->hasPowerLimit && powerTargetWrittenByReset) {
        // The reset-to-stock phase already put the board on this apply's own
        // power target instead of passing through the board default first, so
        // the domain is satisfied before the VF curve was ever raised.  It is
        // still counted and still refreshes state, exactly as the late write
        // used to -- only the timing moved.
        successCount++;
        powerChanged = true;
        debug_log("apply power limit: %d%% already established by the reset phase"
                  " (before the curve write), no second write\n",
            desired->powerLimitPct);
    } else if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        const bool forcePowerWrite = g_app.powerLimitWriteUncertain;
        if (forcePowerWrite || desired->powerLimitPct != currentPowerPct) {
            if (desired->powerLimitPct != clamp_power_limit_pct(desired->powerLimitPct)) {
                append_failure("Power limit %d%% outside safe range %d..%d%%", desired->powerLimitPct, POWER_LIMIT_MIN_PCT, POWER_LIMIT_MAX_PCT);
                failCount++;
                partialApplyRisk = true;
            } else if (!forcePowerWrite && !power_limit_surface_available(
                           g_app.readback.powerLimit, g_app.powerLimitDefaultmW, g_app.powerLimitCurrentmW)) {
                // A real request for a target this board has no surface for.
                // It fails, loudly and by name — the silent-refusal path is the
                // one this domain was getting wrong, not the refusal itself.
                append_failure("Power limit %d%% cannot be set: this GPU's driver does not"
                               " report a power target (constraints %d..%d mW)",
                    desired->powerLimitPct, g_app.powerLimitMinmW, g_app.powerLimitMaxmW);
                failCount++;
                partialApplyRisk = true;
            } else {
                powerChanged = true;
                set_last_apply_phase("apply: power limit write");
                if (nvapi_set_power_limit(desired->powerLimitPct, forcePowerWrite))
                    successCount++;
                else {
                    failCount++;
                    partialApplyRisk = true;
                    append_failure("Power limit %d%% was not accepted by the driver", desired->powerLimitPct);
                }
            }
        }
    }
}
#endif
