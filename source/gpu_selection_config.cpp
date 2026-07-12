// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Durable selected-GPU config I/O. Included by gpu_backend.cpp after the shared
// PCI identity helpers have been defined.

static bool load_selected_gpu_from_config(ConfiguredGpuSelection* configured) {
    if (!configured) return false;
    memset(configured, 0, sizeof(*configured));
    if (g_app.selectedGpuExplicit) {
        configured->legacyIndex = g_app.selectedGpuIndex;
        return true;
    }
    char err[256] = {};
    if (!load_configured_gpu_selection(g_app.configPath, configured,
            err, sizeof(err))) {
        debug_log("gpu selection: config is incoherent: %s\n",
            err[0] ? err : "unknown error");
        g_app.configuredGpuSelectionUnresolved = true;
        return false;
    }
    g_app.selectedGpuIndex = configured->legacyIndex;
    g_app.selectedNvmlIndex = configured->legacyIndex;
    g_app.selectedGpuExplicit = true;
    return true;
}

static bool save_configured_gpu_selection_atomic(unsigned int index,
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g_app.configPath[0] || index >= g_app.adapterCount ||
        index >= MAX_GPU_ADAPTERS || !g_app.adapters[index].valid) {
        set_message(err, errSize, "Selected GPU is not available");
        return false;
    }
    const GpuAdapterInfo& target = g_app.adapters[index];
    const bool stableIdentity = target.pciInfoValid;
    const bool hasBdf = gpu_adapter_has_valid_pci_location(&target);
    if (g_app.adapterCount > 1 && !stableIdentity) {
        set_message(err, errSize,
            "Cannot persist a multi-GPU selection without stable PCI identity");
        return false;
    }
    if (g_app.adapterCount > 1 && stableIdentity && !hasBdf &&
        !gpu_adapter_pci_base_identity_is_unique(
            &target, g_app.adapters, g_app.adapterCount)) {
        set_message(err, errSize,
            "Cannot persist an ambiguous identical-GPU selection without PCI location");
        return false;
    }

    ConfiguredGpuSelection configured = {};
    configured.legacyIndex = index;
    configured.stableIdentityPresent = stableIdentity;
    if (stableIdentity) configured.identity = target;
    char section[1024] = {};
    if (!format_configured_gpu_selection_section("gpu", &configured,
            section, ARRAY_COUNT(section), err, errSize)) {
        return false;
    }

    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) {
        set_message(err, errSize,
            "Failed to acquire the config lock for GPU selection");
        return false;
    }
    const char* replaced[] = { "gpu" };
    bool ok = write_config_sections_atomic(g_app.configPath, section,
        replaced, ARRAY_COUNT(replaced), err, errSize);
    if (ok) {
        (void)WritePrivateProfileStringA(nullptr, nullptr, nullptr,
            g_app.configPath);
        ConfiguredGpuSelection readback = {};
        char verifyErr[256] = {};
        unsigned int resolved = 0;
        ConfiguredGpuResolveResult verifyResult =
            CONFIGURED_GPU_RESOLVE_NOT_FOUND;
        bool loaded = load_configured_gpu_selection(g_app.configPath, &readback,
            verifyErr, sizeof(verifyErr));
        if (loaded) {
            verifyResult = resolve_configured_gpu_selection(&readback,
                g_app.adapters, g_app.adapterCount, &resolved);
        }
        ok = loaded && resolved == index &&
            (stableIdentity
                ? verifyResult == CONFIGURED_GPU_RESOLVE_STABLE
                : verifyResult == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL);
        if (!ok) {
            set_message(err, errSize,
                "GPU selection was written but failed locked identity readback: %s",
                verifyErr[0] ? verifyErr : "identity mismatch");
        }
    }
    leave_config_storage_lock(configMutex);
    if (ok) invalidate_tray_profile_cache();
    return ok;
}

static bool validate_configured_gpu_selection_for_client(
    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    ConfiguredGpuSelection configured = {};
    if (!load_configured_gpu_selection(g_app.configPath, &configured,
            err, errSize)) {
        g_app.configuredGpuSelectionUnresolved = true;
        return false;
    }
    unsigned int resolved = configured.legacyIndex;
    ConfiguredGpuResolveResult resolution = resolve_configured_gpu_selection(
        &configured, g_app.adapters, g_app.adapterCount, &resolved);
    if ((resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL &&
            g_app.adapterCount > 1) ||
        resolution == CONFIGURED_GPU_RESOLVE_NOT_FOUND ||
        resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS ||
        resolved >= g_app.adapterCount || resolved >= MAX_GPU_ADAPTERS ||
        !g_app.adapters[resolved].valid) {
        g_app.configuredGpuSelectionUnresolved = true;
        set_message(err, errSize,
            resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL
                ? "Legacy GPU ordinal is unsafe with multiple adapters; select the intended GPU again"
                : "The configured GPU identity is missing or ambiguous; select the intended GPU again");
        return false;
    }
    const GpuAdapterInfo& target = g_app.adapters[resolved];
    g_app.selectedGpuIndex = target.nvapiIndex;
    g_app.selectedNvmlIndex = target.nvmlIndex;
    g_app.selectedGpu = target;
    g_app.selectedGpuIdentityValid = true;
    g_app.selectedGpuExplicit = true;
    g_app.selectedGpuOrdinalFallback =
        resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL;
    g_app.configuredGpuSelectionUnresolved = false;
    return true;
}
