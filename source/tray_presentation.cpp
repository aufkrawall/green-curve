// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Tray icon presentation: which profile the tooltip names, whether live GPU
// state may be reported at all, and the assembled tooltip text.  Split out of
// main_gpu_front.cpp, which was at its size ratchet, and included from the exact
// position those functions occupied so the amalgamated ordering is unchanged.
//
// The tooltip reports the profile ACTUALLY APPLIED to the GPU.  Loading a
// profile deliberately does not touch hardware, so the combo's selected_slot is
// an editing selection and must never be reported as active.

static bool live_state_has_custom_oc() {
    bool curveOffsets = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.freqOffsets[i] != 0) {
            curveOffsets = true;
            break;
        }
    }
    return gui_tray_live_state_has_custom_oc(
        g_app.gpuClockOffsetkHz != 0,
        g_app.memClockOffsetkHz != 0,
        g_app.powerLimitPct != 100,
        curveOffsets,
        g_app.appliedLockMode != LOCK_MODE_NONE &&
            g_app.appliedLockFreq > 0);
}
static bool live_state_has_custom_fan() {
    return current_green_curve_fan_intent_mode() != FAN_MODE_AUTO;
}

static GuiTrayProfileKind tray_profile_kind_from_service_source(
    ServiceProfileSource source) {
    switch (source) {
        case SERVICE_PROFILE_SOURCE_USER_SLOT: return GUI_TRAY_PROFILE_USER_SLOT;
        case SERVICE_PROFILE_SOURCE_SHARED_SLOT: return GUI_TRAY_PROFILE_SHARED_SLOT;
        case SERVICE_PROFILE_SOURCE_MACHINE_SLOT: return GUI_TRAY_PROFILE_MACHINE_SLOT;
        case SERVICE_PROFILE_SOURCE_AD_HOC: return GUI_TRAY_PROFILE_AD_HOC;
        default: return GUI_TRAY_PROFILE_NONE;
    }
}
// Reports the profile actually applied to the GPU. `[profiles] applied_slot` is
// written by every apply path and is the same authority the tray menu checkmark
// reads, so the tooltip and the tick cannot disagree. The combo's selected_slot
// is an EDITING selection -- loading a profile does not touch hardware -- and
// must never be reported as active. A "(saved)"/"(empty)" suffix is likewise
// gone: an applied slot is saved by definition.
static void ensure_tray_profile_cache() {
    if (g_app.trayProfileCacheValid) return;
    g_app.trayProfileCacheValid = true;
    g_app.trayProfileCacheProfilePart[0] = 0;
    int appliedSlot = 0;
    if (g_app.configPath[0] != '\0') {
        appliedSlot = get_config_int(g_app.configPath, "profiles", "applied_slot", 0);
        if (appliedSlot < 1 || appliedSlot > CONFIG_NUM_SLOTS) appliedSlot = 0;
    }
    GuiTrayProfileKind kind = GUI_TRAY_PROFILE_NONE;
    int sourceSlot = 0;
    if (g_app.serviceSnapshotAuthoritative) {
        kind = tray_profile_kind_from_service_source(g_app.serviceActiveProfileSource);
        sourceSlot = (int)g_app.serviceActiveProfileSlot;
    }
    gui_tray_format_active_profile(
        g_app.trayProfileCacheProfilePart,
        ARRAY_COUNT(g_app.trayProfileCacheProfilePart),
        appliedSlot, kind, sourceSlot, CONFIG_NUM_SLOTS);
    debug_log_on_change("tray profile: applied slot=%d source=%u sourceSlot=%d authoritative=%d -> \"%s\"\n",
        appliedSlot, (unsigned int)g_app.serviceActiveProfileSource, sourceSlot,
        g_app.serviceSnapshotAuthoritative ? 1 : 0,
        g_app.trayProfileCacheProfilePart);
}
// True from the moment a profile switch / Apply is queued until the service's
// result has been adopted.  `applyInFlight` is owned by the GUI mutation queue
// (gui_mutation_worker.cpp), which is the single serialization point every
// apply path -- manual Apply, tray/hotkey profile pick, app-start apply --
// passes through, so no path can leave this surface behind.  The service binary
// never queues client mutations, so this is constantly false there.
static bool tray_apply_in_flight() {
    return g_app.applyInFlight;
}
// True when the GPU live state is actually available, so the snapshot's OC/fan
// reflect real applied hardware state rather than a pending desired profile while
// the driver is disabled/removed or the service is down.  Shared by the tray icon
// theme and the tray tooltip so both stay consistent.
static bool tray_hardware_live() {
    if (g_app.usingBackgroundService && !g_app.isServiceProcess)
        return gui_service_model_ready(&g_app.guiServiceModel) && g_app.loaded;
    return g_app.loaded;
}
static void build_tray_tooltip(char* tip, size_t tipSize) {
    if (!tip || tipSize == 0) return;
    ensure_tray_profile_cache();
    if (tray_apply_in_flight()) {
        // Same precedence as the icon theme: while the write is in flight the
        // previous profile is no longer what is being asked for and the new one
        // is not in effect yet, so name neither.
        gui_apply_in_flight_tray_tooltip(tip, tipSize);
        return;
    }
    if (!tray_hardware_live()) {
        // GPU live state unavailable: nothing is actually applied, so do not report
        // OC/fan/profile as active (matches the default tray icon theme).
        StringCchPrintfA(tip, tipSize, "Green Curve - %s%s",
            gui_service_phase_tray_text(g_app.guiServiceModel.phase),
            g_app.guiStateDirty ? " | unsaved draft preserved" : "");
        return;
    }
    char mode[64] = {};
    bool customOc = live_state_has_custom_oc();
    bool customFan = live_state_has_custom_fan();
    StringCchCopyA(mode, ARRAY_COUNT(mode), tray_mode_label(customOc, customFan));
    // The fallback must not assert a profile either: an empty cache means the
    // active one is unknown, not that slot 1 is running.
    const char* profilePart = g_app.trayProfileCacheProfilePart[0]
        ? g_app.trayProfileCacheProfilePart
        : "No profile";
    StringCchPrintfA(tip, tipSize, "Green Curve - %s | %s", mode, profilePart);
}
