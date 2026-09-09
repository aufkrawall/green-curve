// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The board power-target (TGP) read and write pair, split out of
// gpu_backend.cpp when it passed its size ratchet, and included from the
// position it occupied so the amalgamated ordering is unchanged.
//
// The pair belongs together because they share one fragile contract: Green
// Curve expresses this domain as a PERCENTAGE OF THE BOARD DEFAULT, so the
// write is only meaningful once the read has established a default in mW.  A
// driver that refuses either number leaves the domain with no control surface
// at all, and every consumer must be able to tell that apart from a measured
// value -- which is what power_limit_surface_available() in
// control_readback_policy.h exists to answer.  Getting that wrong is not
// cosmetic: a fabricated 0% made the reset-to-stock step believe the target was
// 100 points off stock and issue a write the driver could only refuse, failing
// the entire Apply (VF curve included) on hardware whose curve was perfectly
// writable.  Reported 2026-09-07 against 0.25.0 on an RTX 3060 Laptop GPU.

// Read the board's power target.  Three NVML calls, all independently
// optional, because they are independently refused in the wild: notebook boards
// whose TGP the OEM/EC owns commonly answer the *constraints* while refusing
// the limit and/or the default limit.  Every failure path here is logged with
// the exact NVML status, because the only symptom this used to produce -- the
// whole Apply refused with "Power target did not reset" -- named neither the
// call that failed nor the domain that could not answer.
static bool nvml_read_power_limit() {
    g_app.readback.powerLimit = false;
    if (!nvml_ensure_ready()) {
        debug_log("read_power_limit: NVML not ready\n");
        return false;
    }
    // Constraints are read FIRST and unconditionally: they drive the advertised
    // OC range and the write-side bounds, and they stay meaningful even when
    // the limit read below is refused.  Reading them last used to make every
    // consumer see 0/0/0 mW on exactly the boards that need them most.
    g_app.powerLimitMinmW = g_app.powerLimitMaxmW = 0;
    if (g_nvml_api.getPowerConstraints) {
        unsigned int mn = 0, mx = 0;
        nvmlReturn_t conRet = g_nvml_api.getPowerConstraints(g_app.nvmlDevice, &mn, &mx);
        if (conRet == NVML_SUCCESS) {
            g_app.powerLimitMinmW = (int)mn;
            g_app.powerLimitMaxmW = (int)mx;
        } else {
            debug_log("read_power_limit: nvmlDeviceGetPowerManagementLimitConstraints failed: %s\n",
                nvml_err_name(conRet));
        }
    }
    // Either getter alone is enough to establish a usable pair; requiring both
    // pointers made a driver that simply does not export the default-limit
    // symbol indistinguishable from one with no power surface at all.
    if (!g_nvml_api.getPowerLimit && !g_nvml_api.getPowerDefaultLimit) {
        g_app.powerLimitCurrentmW = g_app.powerLimitDefaultmW = 0;
        g_app.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
        debug_log("read_power_limit: no power-limit getter exported (getLimit=%d getDefault=%d);"
                  " publishing %d%% as unknown\n",
            g_nvml_api.getPowerLimit ? 1 : 0, g_nvml_api.getPowerDefaultLimit ? 1 : 0,
            POWER_LIMIT_DEFAULT_PCT);
        return false;
    }
    unsigned int cur = 0, def = 0;
    nvmlReturn_t curRet = g_nvml_api.getPowerLimit
        ? g_nvml_api.getPowerLimit(g_app.nvmlDevice, &cur) : NVML_ERROR_NOT_SUPPORTED;
    nvmlReturn_t defRet = g_nvml_api.getPowerDefaultLimit
        ? g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &def) : NVML_ERROR_NOT_SUPPORTED;
    if (curRet != NVML_SUCCESS) {
        cur = 0;
        debug_log("read_power_limit: nvmlDeviceGetPowerManagementLimit failed: %s\n",
            nvml_err_name(curRet));
    }
    if (defRet != NVML_SUCCESS) {
        def = 0;
        debug_log("read_power_limit: nvmlDeviceGetPowerManagementDefaultLimit failed: %s\n",
            nvml_err_name(defRet));
    }
    if (cur == 0 && def == 0) {
        // No usable pair.  Publish the neutral board-default percentage and a
        // FALSE readback so every consumer -- editor, IPC clamp, reset baseline,
        // readback comparison -- treats the domain as unknown rather than as a
        // measured 0%.  See power_limit_surface_available().
        g_app.powerLimitCurrentmW = g_app.powerLimitDefaultmW = 0;
        g_app.powerLimitPct = POWER_LIMIT_DEFAULT_PCT;
        debug_log("read_power_limit: power target unreadable on this board"
                  " (constraints %d..%d mW); publishing %d%% as unknown,"
                  " power writes will be refused loudly\n",
            g_app.powerLimitMinmW, g_app.powerLimitMaxmW, POWER_LIMIT_DEFAULT_PCT);
        return false;
    }
    // One answered getter still fixes the pair: an unknown default is the
    // current limit (the board is at its default until proven otherwise), and
    // an unknown current is the default.
    if (def == 0) def = cur;
    if (cur == 0) cur = def;
    g_app.powerLimitCurrentmW = (int)cur;
    g_app.powerLimitDefaultmW = (int)def;
    g_app.powerLimitPct = power_limit_pct_from_mw(g_app.powerLimitCurrentmW,
                                                  g_app.powerLimitDefaultmW);
    debug_log_on_change("read_power_limit: current=%d mW default=%d mW -> %d%%"
                        " (constraints %d..%d mW, curRet=%s defRet=%s)\n",
        g_app.powerLimitCurrentmW, g_app.powerLimitDefaultmW, g_app.powerLimitPct,
        g_app.powerLimitMinmW, g_app.powerLimitMaxmW,
        nvml_err_name(curRet), nvml_err_name(defRet));
    g_app.readback.powerLimit = true;
    return true;
}
// Every refusal below is logged.  These are the only silent failures the apply
// and reset paths can hit in this domain, and a silent `false` here surfaced as
// nothing but a generic "Power target did not reset" three layers up.
static bool nvapi_set_power_limit(int pct) {
    if (pct < POWER_LIMIT_MIN_PCT || pct > POWER_LIMIT_MAX_PCT) {
        debug_log("set_power_limit: refused pct=%d outside %d..%d%%\n",
            pct, POWER_LIMIT_MIN_PCT, POWER_LIMIT_MAX_PCT);
        return false;
    }
    if (g_app.powerLimitDefaultmW <= 0) {
        debug_log("set_power_limit: refused pct=%d - this board never reported a default"
                  " power limit, so there is no percentage base to write against"
                  " (constraints %d..%d mW)\n",
            pct, g_app.powerLimitMinmW, g_app.powerLimitMaxmW);
        return false;
    }
    unsigned int targetmW = (unsigned int)(((long long)g_app.powerLimitDefaultmW * pct + 50) / 100);
    if (targetmW < 1) {
        debug_log("set_power_limit: refused pct=%d - target rounds to 0 mW from default %d mW\n",
            pct, g_app.powerLimitDefaultmW);
        return false;
    }
    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) {
        debug_log("set_power_limit: refused pct=%d - target %u mW below the board minimum %d mW\n",
            pct, targetmW, g_app.powerLimitMinmW);
        return false;
    }
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) {
        debug_log("set_power_limit: refused pct=%d - target %u mW above the board maximum %d mW\n",
            pct, targetmW, g_app.powerLimitMaxmW);
        return false;
    }
    debug_log("set_power_limit: pct=%d defaultmW=%d targetmW=%u\n", pct, g_app.powerLimitDefaultmW, targetmW);
    if (nvml_ensure_ready() && g_nvml_api.setPowerLimit) {
        set_last_apply_phase("Power limit NVML write");
        nvmlReturn_t r = g_nvml_api.setPowerLimit(g_app.nvmlDevice, targetmW);
        if (r == NVML_SUCCESS) {
            nvml_read_power_limit();
            return true;
        }
        debug_log("Power limit via NVML failed: %s\n", nvml_err_name(r));
    }
    WCHAR exePath[MAX_PATH] = {};
    if (!find_trusted_nvidia_smi_path_w(exePath, ARRAY_COUNT(exePath))) {
        debug_log("Power limit via nvidia-smi skipped: trusted executable not found\n");
        return false;
    }
    int watts = (int)((targetmW + 500) / 1000);
    WCHAR cmdLine[MAX_PATH + 64] = {};
    StringCchPrintfW(cmdLine, ARRAY_COUNT(cmdLine), L"\"%ls\" -pl %d", exePath, watts);
    set_last_apply_phase("Power limit nvidia-smi write");
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    ScopedProcess proc;
    if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("Power limit via nvidia-smi failed to launch (error %lu)\n", GetLastError());
        return false;
    }
    proc.assign(pi.hProcess, pi.hThread);
    DWORD waitResult = proc.wait(5000);
    if (waitResult == WAIT_TIMEOUT) {
        proc.terminate(1);
        proc.wait(1000);
        debug_log("Power limit via nvidia-smi timed out and was terminated\n");
        return false;
    }
    DWORD exitCode = proc.exit_code();
    if (exitCode == 0) {
        nvml_read_power_limit();
    }
    else debug_log("Power limit via nvidia-smi failed with exit code %lu\n", exitCode);
    return exitCode == 0;
}
