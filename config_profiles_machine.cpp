// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Machine-wide profile bank. A published slot contains both its settings and
// the stable GPU identity selected by the administrator at publication time.

static bool machine_profile_section_names(int slot, char* controls,
    size_t controlsSize, char* curve, size_t curveSize, char* fan,
    size_t fanSize, char* gpu, size_t gpuSize) {
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return false;
    return SUCCEEDED(StringCchPrintfA(controls, controlsSize, "profile%d", slot)) &&
        SUCCEEDED(StringCchPrintfA(curve, curveSize, "profile%d_curve", slot)) &&
        SUCCEEDED(StringCchPrintfA(fan, fanSize, "profile%d_fan_curve", slot)) &&
        SUCCEEDED(StringCchPrintfA(gpu, gpuSize, "profile%d_gpu", slot));
}

static bool replace_machine_profile_slot_sections(const char* machinePath,
    int slot, const char* replacement, char* err, size_t errSize) {
    char controls[32] = {}, curve[32] = {}, fan[32] = {}, gpu[32] = {};
    char publish[32] = {};
    if (!machine_profile_section_names(slot, controls, ARRAY_COUNT(controls),
            curve, ARRAY_COUNT(curve), fan, ARRAY_COUNT(fan), gpu,
            ARRAY_COUNT(gpu)) ||
        FAILED(StringCchPrintfA(publish, ARRAY_COUNT(publish),
            "profile%d_publish", slot))) {
        set_message(err, errSize, "Invalid machine profile slot %d", slot);
        return false;
    }
    const char* replaced[8] = { controls, curve, fan, gpu, publish };
    int replaceCount = 5;
    // Slot 1 is also mirrored into legacy sections by the profile writer.
    // Coupled invalidation/clear must remove those aliases or the fallback
    // reader would still see a partially published slot after cleanup.
    if (slot == 1) {
        replaced[replaceCount++] = "controls";
        replaced[replaceCount++] = "curve";
        replaced[replaceCount++] = "fan_curve";
    }
    return write_config_sections_atomic(machinePath,
        replacement ? replacement : "", replaced, replaceCount,
        err, errSize);
}

enum MachineProfilePublishState {
    MACHINE_PROFILE_PUBLISH_LEGACY = 0,
    MACHINE_PROFILE_PUBLISH_IN_PROGRESS,
    MACHINE_PROFILE_PUBLISH_COMMITTED,
    MACHINE_PROFILE_PUBLISH_INVALID,
};

static MachineProfilePublishState machine_profile_publish_state(
    const char* machinePath, int slot) {
    char section[32] = {}, value[32] = {};
    if (!machinePath || slot < 1 || slot > CONFIG_NUM_SLOTS ||
        FAILED(StringCchPrintfA(section, ARRAY_COUNT(section),
            "profile%d_publish", slot)) ||
        !get_config_string(machinePath, section, "state", "", value,
            sizeof(value))) return MACHINE_PROFILE_PUBLISH_INVALID;
    if (!value[0]) return MACHINE_PROFILE_PUBLISH_LEGACY;
    if (_stricmp(value, "publishing") == 0) {
        return MACHINE_PROFILE_PUBLISH_IN_PROGRESS;
    }
    if (_stricmp(value, "committed") == 0) {
        return MACHINE_PROFILE_PUBLISH_COMMITTED;
    }
    return MACHINE_PROFILE_PUBLISH_INVALID;
}

static bool machine_profile_slot_is_coherent(
    const char* machinePath, int slot) {
    if (!machinePath || !is_profile_slot_saved(machinePath, slot)) return false;
    MachineProfilePublishState state =
        machine_profile_publish_state(machinePath, slot);
    char controls[32] = {}, curve[32] = {}, fan[32] = {}, gpu[32] = {};
    ConfiguredGpuSelection selection = {};
    char err[256] = {};
    if (!machine_profile_section_names(slot, controls, ARRAY_COUNT(controls),
            curve, ARRAY_COUNT(curve), fan, ARRAY_COUNT(fan), gpu,
            ARRAY_COUNT(gpu))) return false;
    bool gpuBindingPresent = config_section_has_keys(machinePath, gpu);
    if (state == MACHINE_PROFILE_PUBLISH_LEGACY) {
        // Pre-marker banks may also predate GPU binding entirely. Absence is
        // handled by the single-adapter-only compatibility path; a present but
        // malformed binding is never downgraded to an ordinal.
        return !gpuBindingPresent ||
            load_configured_gpu_selection_from_section(machinePath, gpu,
                &selection, err, sizeof(err));
    }
    return state == MACHINE_PROFILE_PUBLISH_COMMITTED && gpuBindingPresent &&
        load_configured_gpu_selection_from_section(machinePath, gpu,
            &selection, err, sizeof(err));
}

static bool configured_gpu_selections_equal(
    const ConfiguredGpuSelection* a, const ConfiguredGpuSelection* b) {
    if (!a || !b || a->legacyIndex != b->legacyIndex ||
        a->stableIdentityPresent != b->stableIdentityPresent) return false;
    if (!a->stableIdentityPresent) return true;
    return configured_gpu_base_identity_matches(&a->identity, &b->identity) &&
        configured_gpu_identity_has_bdf(&a->identity) ==
            configured_gpu_identity_has_bdf(&b->identity) &&
        (!configured_gpu_identity_has_bdf(&a->identity) ||
         (a->identity.pciDomain == b->identity.pciDomain &&
          a->identity.pciBus == b->identity.pciBus &&
          a->identity.pciDevice == b->identity.pciDevice &&
          a->identity.pciFunction == b->identity.pciFunction));
}

bool is_machine_profile_slot_saved(int slot) {
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return false;
    char machinePath[MAX_PATH] = {};
    return resolve_machine_config_path(machinePath, sizeof(machinePath)) &&
        machine_profile_slot_is_coherent(machinePath, slot);
}

bool load_machine_profile_gpu_selection(int slot,
    ConfiguredGpuSelection* selectionOut, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!selectionOut || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid machine profile GPU selection");
        return false;
    }
    char machinePath[MAX_PATH] = {}, controls[32] = {}, curve[32] = {};
    char fan[32] = {}, gpu[32] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath)) ||
        !machine_profile_section_names(slot, controls, ARRAY_COUNT(controls),
            curve, ARRAY_COUNT(curve), fan, ARRAY_COUNT(fan), gpu,
            ARRAY_COUNT(gpu))) {
        set_message(err, errSize, "Cannot resolve machine profile GPU selection");
        return false;
    }
    MachineProfilePublishState publishState =
        machine_profile_publish_state(machinePath, slot);
    if (publishState == MACHINE_PROFILE_PUBLISH_IN_PROGRESS ||
        publishState == MACHINE_PROFILE_PUBLISH_INVALID) {
        set_message(err, errSize,
            "Shared slot %d publication is incomplete", slot);
        return false;
    }
    // The generic parser intentionally treats an absent section as a legacy
    // ordinal-zero config. Distinguish absence here so callers can apply the
    // explicit single-adapter-only compatibility policy.
    if (!config_section_has_keys(machinePath, gpu)) {
        set_message(err, errSize,
            "Shared slot %d has no published GPU binding", slot);
        return false;
    }
    return load_configured_gpu_selection_from_section(machinePath, gpu,
        selectionOut, err, errSize);
}

bool copy_profile_slot_to_machine_config(const char* srcPath, int slot,
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!srcPath || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid machine profile copy arguments");
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize,
            "Publishing a profile to the machine-wide bank requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }

    ConfigStorageLockGuard transactionLock;
    if (!transactionLock.locked()) {
        set_message(err, errSize,
            "Failed to acquire the shared-profile publication lock");
        return false;
    }
    if (!is_profile_slot_saved(srcPath, slot)) {
        set_message(err, errSize,
            "Slot %d is empty; there is no profile to publish", slot);
        return false;
    }
    DesiredSettings desired = {};
    ConfiguredGpuSelection configuredGpu = {};
    char detail[256] = {};
    if (!load_profile_from_config(srcPath, slot, &desired,
            detail, sizeof(detail))) {
        set_message(err, errSize, "Failed loading source profile: %s",
            detail[0] ? detail : "unknown");
        return false;
    }
    if (!config_section_has_keys(srcPath, "gpu") ||
        !load_configured_gpu_selection(srcPath, &configuredGpu,
            detail, sizeof(detail))) {
        set_message(err, errSize,
            "Cannot publish slot %d without its selected GPU: %s", slot,
            detail[0] ? detail : "select the intended GPU again");
        return false;
    }

    // Atomically invalidate the slot and leave a durable in-progress marker
    // before the multi-step staging writes. If this process crashes, readers
    // reject the slot instead of pairing new settings with an old GPU binding.
    char publishingSection[128] = {};
    if (FAILED(StringCchPrintfA(publishingSection,
            ARRAY_COUNT(publishingSection),
            "[profile%d_publish]\r\nstate=publishing\r\n\r\n", slot)) ||
        !replace_machine_profile_slot_sections(machinePath, slot,
            publishingSection,
            err, errSize)) return false;
    WCHAR machinePathW[MAX_PATH] = {};
    if (!utf8_to_wide(machinePath, machinePathW, ARRAY_COUNT(machinePathW)) ||
        !harden_machine_config_file_required(machinePathW, machinePath,
            err, errSize)) {
        // The durable publishing marker already makes the slot unavailable.
        // Best-effort cleanup may remove it, but failure cannot expose data.
        char cleanupErr[256] = {};
        bool cleaned = replace_machine_profile_slot_sections(machinePath, slot,
            "", cleanupErr, sizeof(cleanupErr));
        debug_log("machine profile bank: slot %d file hardening failed before publication; cleanup=%d detail=%s\n",
            slot, cleaned ? 1 : 0,
            cleanupErr[0] ? cleanupErr : "none");
        return false;
    }

    bool ok = save_profile_to_config(machinePath, slot, &desired,
        detail, sizeof(detail));
    char gpuSectionName[32] = {}, controls[32] = {}, curve[32] = {}, fan[32] = {};
    char gpuSection[1024] = {};
    if (ok) {
        ok = machine_profile_section_names(slot, controls, ARRAY_COUNT(controls),
            curve, ARRAY_COUNT(curve), fan, ARRAY_COUNT(fan), gpuSectionName,
            ARRAY_COUNT(gpuSectionName)) &&
            format_configured_gpu_selection_section(gpuSectionName,
                &configuredGpu, gpuSection, ARRAY_COUNT(gpuSection),
                detail, sizeof(detail));
    }
    if (ok) {
        const char* replaced[] = { gpuSectionName };
        ok = write_config_sections_atomic(machinePath, gpuSection, replaced,
            ARRAY_COUNT(replaced), detail, sizeof(detail));
    }
    ConfiguredGpuSelection readback = {};
    if (ok) {
        ok = load_configured_gpu_selection_from_section(machinePath,
                gpuSectionName, &readback, detail, sizeof(detail)) &&
            configured_gpu_selections_equal(&configuredGpu, &readback);
        if (!ok && !detail[0]) {
            StringCchCopyA(detail, ARRAY_COUNT(detail),
                "published GPU identity readback mismatch");
        }
    }
    if (ok) {
        char publishSectionName[32] = {}, committedSection[128] = {};
        ok = SUCCEEDED(StringCchPrintfA(publishSectionName,
                ARRAY_COUNT(publishSectionName), "profile%d_publish", slot)) &&
            SUCCEEDED(StringCchPrintfA(committedSection,
                ARRAY_COUNT(committedSection),
                "[%s]\r\nstate=committed\r\n\r\n",
                publishSectionName));
        if (ok) {
            const char* replaced[] = { publishSectionName };
            ok = write_config_sections_atomic(machinePath, committedSection,
                replaced, ARRAY_COUNT(replaced), detail, sizeof(detail));
        }
        if (ok) {
            ok = machine_profile_publish_state(machinePath, slot) ==
                MACHINE_PROFILE_PUBLISH_COMMITTED;
            if (!ok) {
                StringCchCopyA(detail, ARRAY_COUNT(detail),
                    "published slot commit marker readback mismatch");
            }
        }
    }
    if (!ok) {
        char cleanupErr[256] = {};
        bool cleaned = replace_machine_profile_slot_sections(machinePath,
            slot, "", cleanupErr, sizeof(cleanupErr));
        debug_log("machine profile bank: publication slot %d failed (%s); fail-closed cleanup=%d detail=%s\n",
            slot, detail[0] ? detail : "unknown", cleaned ? 1 : 0,
            cleanupErr[0] ? cleanupErr : "none");
        set_message(err, errSize, "Failed publishing machine profile: %s",
            detail[0] ? detail : "unknown");
        return false;
    }

    debug_log("machine profile bank: published slot %d with GPU legacy=%u stable=%d bdf=%d from %s to %s\n",
        slot, configuredGpu.legacyIndex,
        configuredGpu.stableIdentityPresent ? 1 : 0,
        configured_gpu_identity_has_bdf(&configuredGpu.identity) ? 1 : 0,
        srcPath, machinePath);
    return true;
}

bool clear_machine_profile_slot(int slot, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid machine profile slot %d", slot);
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize,
            "Clearing a machine-wide profile slot requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    ConfigStorageLockGuard transactionLock;
    if (!transactionLock.locked() ||
        !replace_machine_profile_slot_sections(machinePath, slot, "",
            err, errSize)) return false;
    WCHAR machinePathW[MAX_PATH] = {};
    if (!utf8_to_wide(machinePath, machinePathW, ARRAY_COUNT(machinePathW))) {
        set_message(err, errSize, "Machine config path conversion failed");
        return false;
    }
    if (!harden_machine_config_file_required(machinePathW, machinePath,
            err, errSize)) return false;
    invalidate_tray_profile_cache();
    debug_log("machine profile bank: atomically cleared slot %d settings and GPU binding from %s\n",
        slot, machinePath);
    return true;
}

bool share_profile_slot_for_all_users(const char* srcPath, int slot,
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!srcPath || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid share arguments");
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize,
            "Sharing a profile with all users requires administrator rights");
        return false;
    }
    if (!copy_profile_slot_to_machine_config(srcPath, slot, err, errSize) ||
        !set_machine_logon_slot(slot, err, errSize)) return false;
    debug_log("share: slot %d settings and GPU binding published; all-users default selected\n",
        slot);
    return true;
}

bool unshare_profile_slot_for_all_users(int slot, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid unshare slot %d", slot);
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize,
            "Unsharing a profile requires administrator rights");
        return false;
    }
    int machineSlot = 0;
    if (get_machine_logon_slot(&machineSlot) && machineSlot == slot &&
        !clear_machine_logon_slot(err, errSize)) return false;
    if (!clear_machine_profile_slot(slot, err, errSize)) return false;
    debug_log("unshare: slot %d removed from the shared bank (default cleared if it pointed here)\n",
        slot);
    return true;
}
