// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "gpu_backend_apply.cpp"
static bool apply_desired_settings(const DesiredSettings* desired, bool interactive,
    ServiceApplyOrigin origin, ServiceProfileSource profileSource, int profileSlot,
    char* result, size_t resultSize) {
    if (!g_app.isServiceProcess) {
        refresh_background_service_state();
        if (!g_app.backgroundServiceAvailable) {
            set_message(result, resultSize,
                g_app.backgroundServiceInstalled
                    ? "Background service is not responding. Live GPU control is unavailable until it starts responding again."
                    : "Background service is not installed. Install it to enable live GPU control.");
            return false;
        }
        ServiceSnapshot snapshot = {};
        bool ok = service_client_apply_desired(desired,
            g_pendingOperationSource[0] ? g_pendingOperationSource : "client apply",
            interactive, origin, profileSource, profileSlot,
            result, resultSize, &snapshot);
        if (snapshot.initialized || snapshot.loaded) {
            bool fanOnlyApply = desired_is_fan_only_apply_request(desired);
            apply_service_snapshot_to_app(&snapshot);
            // Record the drift-free curve intent we just applied so the editor/graph,
            // fan-only detection, and saves use it instead of live boost/temperature
            // drift. Fan-only requests carry no curve intent, so leave the baseline as
            // it is (do not clear the curve the service still holds).
            if (ok && !fanOnlyApply) capture_applied_curve_baseline(desired);
            if (desired && desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && desired->lockMHz > 0) {
                g_app.lockedCi = desired->lockCi; g_app.lockedFreq = desired->lockMHz; g_app.lockMode = desired->lockMode; g_app.guiLockTracksAnchor = desired->lockTracksAnchor; g_app.lockedVi = -1;
                for (int vi = 0; vi < g_app.numVisible; vi++) {
                    if (g_app.visibleMap[vi] == desired->lockCi) {
                        g_app.lockedVi = vi;
                        break;
                    }
                }
                g_app.appliedLockVi = g_app.lockedVi; g_app.appliedLockCi = g_app.lockedCi; g_app.appliedLockFreq = g_app.lockedFreq; g_app.appliedLockMode = g_app.lockMode;
                debug_log("service apply client lock sync: ci=%d requestedMHz=%u trackAnchor=%d\n", g_app.lockedCi, g_app.lockedFreq, g_app.guiLockTracksAnchor ? 1 : 0);
            }
            if (g_app.hMainWnd) {
                populate_global_controls(); if (g_app.loaded && !fanOnlyApply) populate_edits();
                else if (fanOnlyApply) debug_log("service apply client: skipped VF edit repaint for fan-only apply to preserve sparse curve intent\n");
                invalidate_main_window(); update_tray_icon();
            }
        }
        return ok;
    }
    return apply_desired_settings_service(desired, interactive, result, resultSize);
}
// ============================================================================
// NvAPI Interface
// ============================================================================

// Module-level cache so close_nvapi() can invalidate it across calls.
// A driver recovery never attempts to reload NvAPI in the stale process; the
// nonce-bound helper starts a fresh service process instead.
static void* (*g_nvapiQi)(unsigned int) = nullptr;

static void* nvapi_qi(unsigned int id) {
    if (!g_nvapiQi) {
        g_app.hNvApi = load_system_library_a("nvapi64.dll");
        if (!g_app.hNvApi) {
            g_app.hNvApi = load_system_library_a("nvapi.dll");
        }
        if (!g_app.hNvApi) return nullptr;
        g_nvapiQi = (void* (*)(unsigned int))GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!g_nvapiQi) return nullptr;
    }
    return g_nvapiQi(id);
}
static bool nvapi_init() {
    typedef int (*init_t)();
    auto init = (init_t)nvapi_qi(NVAPI_INIT_ID);
    if (!init) {
        debug_log("nvapi_init: FAILED — nvapi_QueryInterface(id=NVAPI_INIT_ID) returned null"
            " (dll=%s)\n",
            g_app.hNvApi ? (GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface") ? "loaded" : "qi-missing") : "not-loaded");
        return false;
    }
    int r = init();
    debug_log("nvapi_init: result=%d dll=%s\n",
        r,
        g_app.hNvApi ? "loaded" : "none");
    return r == 0;
}

static void close_nvapi() {
    if (g_app.hNvApi) {
        FreeLibrary(g_app.hNvApi);
        g_app.hNvApi = nullptr;
    }
    // Invalidate the nvapi_qi() module-level cache so the next call
    // reloads nvapi64.dll with fresh function pointers.  Without this,
    // after a GPU driver restart (VEH crash), the cached pointer points
    // to unmapped memory and causes an access violation.
    g_nvapiQi = nullptr;
    // Clear adapter cache so re-enumeration is clean
    g_app.selectedGpuIndex = 0;
    g_app.selectedGpuIdentityValid = false;
    g_app.selectedGpuExplicit = false;
    memset(g_app.adapters, 0, sizeof(g_app.adapters));
    g_app.adapterCount = 0;
    // Clear GPU handle and hardware-init guards so the next call to
    // hardware_initialize() fully re-enumerates GPUs instead of returning
    // early with stale handles from the previous driver session.
    g_app.gpuHandle = nullptr;

    g_app.loaded = false;
    g_app.vfBackend = nullptr;
    debug_log("nvapi: closed DLL, cleared adapter cache, and invalidated nvapi_qi cache\n");
}

static bool gpu_adapter_has_valid_pci_location(const GpuAdapterInfo* adapter) {
    return adapter && adapter->pciDomain <= 0xFFFFu &&
        adapter->pciBus <= 255u && adapter->pciDevice <= 31u &&
        adapter->pciFunction <= 7u &&
        (adapter->pciDomain != 0 || adapter->pciBus != 0 ||
         adapter->pciDevice != 0 || adapter->pciFunction != 0);
}

static bool gpu_adapter_has_same_pci_base_identity(
    const GpuAdapterInfo* a, const GpuAdapterInfo* b) {
    return a && b && a->valid && b->valid && a->pciInfoValid &&
        b->pciInfoValid && a->deviceId == b->deviceId &&
        a->subSystemId == b->subSystemId &&
        a->pciRevisionId == b->pciRevisionId &&
        a->extDeviceId == b->extDeviceId;
}

static bool gpu_adapter_has_same_pci_identity(const GpuAdapterInfo* a, const GpuAdapterInfo* b) {
    if (!gpu_adapter_has_same_pci_base_identity(a, b)) return false;
    bool aHasBdf = gpu_adapter_has_valid_pci_location(a);
    bool bHasBdf = gpu_adapter_has_valid_pci_location(b);
    // BDF is a strengthening field, not a wire-compatibility requirement.
    // Older/current client snapshots can carry the stable PCI IDs without the
    // service-side NVML/NVAPI location enrichment.  Reject a different BDF
    // when both sides know it, but do not make ordinary explicit Apply fail
    // merely because exactly one side has already been enriched.  Durable
    // recovery still counts all base-ID matches and rejects duplicates.
    if (!aHasBdf || !bHasBdf) return true;
    return a->pciDomain == b->pciDomain && a->pciBus == b->pciBus &&
        a->pciDevice == b->pciDevice &&
        a->pciFunction == b->pciFunction;
}

static bool gpu_adapter_pci_base_identity_is_unique(
    const GpuAdapterInfo* target, const GpuAdapterInfo* adapters,
    unsigned int adapterCount) {
    if (!target || !adapters || !target->pciInfoValid) return false;
    unsigned int matches = 0;
    for (unsigned int i = 0; i < adapterCount; ++i) {
        if (adapters[i].valid &&
            gpu_adapter_has_same_pci_base_identity(target, &adapters[i])) {
            ++matches;
        }
    }
    return matches == 1;
}

static void format_gpu_adapter_label(const GpuAdapterInfo* adapter, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    const char* name = adapter && adapter->name[0] ? adapter->name : "NVIDIA GPU";
    if (!adapter || !adapter->valid) {
        StringCchCopyA(out, outSize, name);
        return;
    }
    if (adapter->pciInfoValid) {
        StringCchPrintfA(out, outSize, "%u: %s [%08X/%08X]",
            adapter->nvapiIndex,
            name,
            adapter->deviceId,
            adapter->subSystemId);
    } else {
        StringCchPrintfA(out, outSize, "%u: %s", adapter->nvapiIndex, name);
    }
}

#include "gpu_selection_config.cpp"

static void reset_gpu_runtime_selection() {
    g_app.gpuHandle = nullptr;
    g_app.loaded = false;
    g_app.nvmlReady = false;
    g_app.nvmlDevice = nullptr;
    g_app.vfBackend = nullptr;
    g_app.gpuName[0] = 0;
    g_app.adapterCount = 0;
    memset(g_app.adapters, 0, sizeof(g_app.adapters));
    memset(&g_app.selectedGpu, 0, sizeof(g_app.selectedGpu));
}

static bool nvapi_enum_gpu() {
    typedef int (*enum_t)(GPU_HANDLE*, int*);
    typedef int (*bus_id_t)(GPU_HANDLE, unsigned int*);
    auto enumGpu = (enum_t)nvapi_qi(NVAPI_ENUM_GPU_ID);
    if (!enumGpu) return false;
    auto getBusId = (bus_id_t)nvapi_qi(NVAPI_GPU_GET_BUS_ID_ID);
    auto getBusSlotId =
        (bus_id_t)nvapi_qi(NVAPI_GPU_GET_BUS_SLOT_ID_ID);
    int count = 0;
    GPU_HANDLE handles[64] = {};
    int ret = enumGpu(handles, &count);
    if (ret != 0 || count < 1) return false;
    ConfiguredGpuSelection configured = {};
    bool selectionCoherent = load_selected_gpu_from_config(&configured);
    unsigned int adapterCount = (unsigned int)nvmin(count, MAX_GPU_ADAPTERS);
    memset(g_app.adapters, 0, sizeof(g_app.adapters));
    g_app.adapterCount = adapterCount;
    for (unsigned int i = 0; i < adapterCount; i++) {
        g_app.gpuHandle = handles[i];
        GpuAdapterInfo* adapter = &g_app.adapters[i];
        memset(adapter, 0, sizeof(*adapter));
        adapter->valid = true;
        adapter->nvapiIndex = i;
        adapter->nvmlIndex = i;
        nvapi_get_name();
        StringCchCopyA(adapter->name, ARRAY_COUNT(adapter->name), g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        nvapi_read_gpu_metadata();
        adapter->pciInfoValid = g_app.gpuPciInfoValid;
        adapter->deviceId = g_app.gpuDeviceId;
        adapter->subSystemId = g_app.gpuSubSystemId;
        adapter->pciRevisionId = g_app.gpuPciRevisionId;
        adapter->extDeviceId = g_app.gpuExtDeviceId;
        unsigned int bus = 0;
        unsigned int slot = 0;
        if (getBusId && getBusSlotId &&
            getBusId(handles[i], &bus) == 0 &&
            getBusSlotId(handles[i], &slot) == 0 &&
            bus <= 255u && slot <= 31u) {
            adapter->pciDomain = 0;
            adapter->pciBus = bus;
            adapter->pciDevice = slot;
            adapter->pciFunction = 0;
            debug_log("gpu enumeration: nvapi=%u PCI location 0000:%02X:%02X.0\n",
                i, bus, slot);
        } else {
            debug_log("gpu enumeration: nvapi=%u PCI location unavailable; identical multi-GPU identity will fail closed\n",
                i);
        }
        adapter->family = g_app.gpuFamily;
        adapter->vfReadSupported = g_app.vfBackend && g_app.vfBackend->readSupported;
        adapter->vfWriteSupported = g_app.vfBackend && g_app.vfBackend->writeSupported;
        adapter->vfBestGuess = vf_backend_is_best_guess(g_app.vfBackend);
    }
    if (selectionCoherent) {
        unsigned int resolved = configured.legacyIndex;
        ConfiguredGpuResolveResult resolution = resolve_configured_gpu_selection(
            &configured, g_app.adapters, adapterCount, &resolved);
        if (resolution == CONFIGURED_GPU_RESOLVE_STABLE) {
            g_app.selectedGpuIndex = resolved;
            g_app.selectedNvmlIndex = resolved;
            g_app.configuredGpuSelectionUnresolved = false;
            debug_log("gpu selection: resolved durable PCI identity to nvapi=%u (saved ordinal=%u)\n",
                resolved, configured.legacyIndex);
        } else if (resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL &&
                   adapterCount > 1) {
            g_app.configuredGpuSelectionUnresolved = true;
            g_app.selectedGpuExplicit = false;
            debug_log("gpu selection: legacy ordinal %u is unsafe with %u adapters; automatic writes are blocked until explicit reselection\n",
                configured.legacyIndex, adapterCount);
        } else if (resolution == CONFIGURED_GPU_RESOLVE_NOT_FOUND ||
                   resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS) {
            g_app.configuredGpuSelectionUnresolved = true;
            g_app.selectedGpuIndex = 0;
            g_app.selectedNvmlIndex = 0;
            g_app.selectedGpuExplicit = false;
            debug_log("gpu selection: durable identity %s; automatic writes are blocked until the user selects a GPU\n",
                resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS
                    ? "is ambiguous" : "is not present");
        }
    }
    if (g_app.selectedGpuIndex >= adapterCount) {
        debug_log("gpu selection: requested index %u outside adapter count %u, falling back to 0\n",
            g_app.selectedGpuIndex,
            adapterCount);
        g_app.selectedGpuIndex = 0;
    }
    g_app.gpuHandle = handles[g_app.selectedGpuIndex];
    g_app.selectedGpu = g_app.adapters[g_app.selectedGpuIndex];
    g_app.selectedGpuIdentityValid = g_app.selectedGpu.valid;
    nvapi_get_name();
    nvapi_read_gpu_metadata();
    return true;
}
static bool nvapi_get_name() {
    typedef int (*name_t)(GPU_HANDLE, char*);
    auto getName = (name_t)nvapi_qi(NVAPI_GET_NAME_ID);
    if (!getName) return false;
    return getName(g_app.gpuHandle, g_app.gpuName) == 0;
}
static bool nvapi_read_curve() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->readSupported) return false;
    auto getStatus = (NvApiFunc)nvapi_qi(backend->getStatusId);
    if (!getStatus) return false;
    unsigned char mask[32] = {};
    unsigned int numClocks = backend->defaultNumClocks;
    if (!nvapi_get_vf_info_cached(mask, &numClocks)) return false;
    HeapBuffer buf(backend->statusBufferSize > 0 ? backend->statusBufferSize : 0x4000);
    if (!buf) return false;
    memset(buf, 0, backend->statusBufferSize);
    {
        const unsigned int version = (backend->statusVersion << 16) | backend->statusBufferSize;
        buf.write_at(0, &version, sizeof(version));
    }
    if (backend->statusMaskOffset + sizeof(mask) > backend->statusBufferSize) return false;
    buf.write_at(backend->statusMaskOffset, mask, sizeof(mask));
    if (backend->statusNumClocksOffset + sizeof(numClocks) <= backend->statusBufferSize) {
        buf.write_at(backend->statusNumClocksOffset, &numClocks, sizeof(numClocks));
    }
    int ret = getStatus(g_app.gpuHandle, buf);
    if (ret != 0) return false;
    g_app.numPopulated = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq = 0, volt = 0;
        unsigned int entryOffset = backend->statusEntriesOffset + (unsigned int)i * backend->statusEntryStride;
        if (entryOffset + 8 > backend->statusBufferSize) {
            g_app.curve[i].freq_kHz = 0;
            g_app.curve[i].volt_uV = 0;
            continue;
        }
        buf.read_at(entryOffset, &freq, sizeof(freq));
        buf.read_at(entryOffset + 4, &volt, sizeof(volt));
        g_app.curve[i].freq_kHz = freq;
        g_app.curve[i].volt_uV = volt;
        if (freq > 0) g_app.numPopulated++;
    }
    g_app.loaded = true;
    return true;
}

static bool parse_mhz_value_prefix(const char* text, unsigned int* valueOut) {
    if (valueOut) *valueOut = 0;
    if (!text) return false;
    while (*text == ' ' || *text == '\t') text++;
    char digits[16] = {};
    size_t count = 0;
    while (text[count] >= '0' && text[count] <= '9') {
        if (count + 1 >= ARRAY_COUNT(digits)) return false;
        digits[count] = text[count];
        count++;
    }
    if (count == 0) return false;
    int parsed = 0;
    if (!parse_int_strict(digits, &parsed) || parsed <= 0) return false;
    if (valueOut) *valueOut = (unsigned int)parsed;
    return true;
}

static void read_nvidia_smi_max_clocks() {
    if (g_app.smiClocksRead) return;
    g_app.smiClocksRead = true;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    ScopedProcess proc;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return;
    proc.assign_pipes(hRead, hWrite);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    WCHAR exePath[MAX_PATH] = {};
    if (!find_trusted_nvidia_smi_path_w(exePath, ARRAY_COUNT(exePath))) {
        debug_log("nvidia-smi clock read skipped: trusted executable not found\n");
        return;
    }
    WCHAR cmd[MAX_PATH + 64] = {};
    StringCchPrintfW(cmd, ARRAY_COUNT(cmd), L"\"%ls\" -q -d CLOCK", exePath);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exePath, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        // Detach pipe handles before assign() so its cleanup() doesn't close
        // them.  The read pipe remains open for the read loop below; the
        // write pipe is owned by the child process and closed manually.
        proc.pipeRead = nullptr;
        proc.pipeWrite = nullptr;
        proc.assign(pi.hProcess, pi.hThread);
        proc.pipeRead = hRead;   // restore so ScopedProcess destructor closes it
        CloseHandle(hWrite);     // child owns hWrite now, close our copy
        char* smiBuf = (char*)malloc(4096);
        if (!smiBuf) return;
        memset(smiBuf, 0, 4096);
        DWORD totalRead = 0;
        bool timedOut = false;
        ULONGLONG startTickMs = GetTickCount64();
        while (totalRead < 4096 - 1) {
            DWORD available = 0;
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD toRead = (DWORD)nvmin((int)available, (int)(4096 - 1 - totalRead));
                DWORD n = 0;
                if (!ReadFile(hRead, smiBuf + totalRead, toRead, &n, nullptr) || n == 0) break;
                totalRead += n;
                continue;
            }
            DWORD waitResult = proc.wait(25);
            if (waitResult == WAIT_OBJECT_0) break;
            if (GetTickCount64() - startTickMs >= 5000) {
                timedOut = true;
                proc.terminate(1);
                break;
            }
        }
        if (timedOut) {
            debug_log("nvidia-smi clock read timed out and was terminated\n");
        }
        proc.wait(1000);
        bool inMaxSection = false;
        char* line = smiBuf;
        while (line && *line) {
            char* nextLine = strchr(line, '\n');
            if (nextLine) { *nextLine = 0; nextLine++; }
            char* cr = strchr(line, '\r');
            if (cr) *cr = 0;
            while (*line == ' ' || *line == '\t') line++;
            if (strstr(line, "Max Clocks")) { inMaxSection = true; line = nextLine; continue; }
            if (inMaxSection && line[0] == '[') inMaxSection = false;
            if (inMaxSection) {
                char* vp = nullptr;
                if ((vp = strstr(line, "Memory")) && (vp = strchr(vp, ':'))) {
                    unsigned int parsedMHz = 0;
                    if (parse_mhz_value_prefix(vp + 1, &parsedMHz)) {
                        g_app.smiMemMaxMHz = parsedMHz;
                    } else {
                        debug_log("nvidia-smi clock read: could not parse memory max clock from \"%s\"\n", line);
                    }
                }
            }
            line = nextLine;
        }
        free(smiBuf);
    }
}
static int uniform_curve_offset_khz() {
    if (!vf_curve_global_gpu_offset_supported()) return 0;
    int values[VF_NUM_POINTS] = {};
    int counts[VF_NUM_POINTS] = {};
    int uniqueCount = 0;
    int populatedCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        populatedCount++;
        int delta = g_app.freqOffsets[i];
        bool found = false;
        for (int j = 0; j < uniqueCount; j++) {
            if (values[j] == delta) {
                counts[j]++;
                found = true;
                break;
            }
        }
        if (!found && uniqueCount < VF_NUM_POINTS) {
            values[uniqueCount] = delta;
            counts[uniqueCount] = 1;
            uniqueCount++;
        }
    }
    if (populatedCount == 0) return 0;
    int bestValue = 0;
    int bestCount = 0;
    for (int i = 0; i < uniqueCount; i++) {
        if (counts[i] > bestCount || (counts[i] == bestCount && abs(values[i]) > abs(bestValue))) {
            bestValue = values[i];
            bestCount = counts[i];
        }
    }
    if (bestCount * 2 < populatedCount) return 0;
    return bestValue;
}
static void detect_clock_offsets() {
    // Detect global offsets from generic driver-visible sources.
    // GPU: only use uniform VF control deltas on write-supported backends where
    // the control-table layout is validated. Probe-only/read-only backends may
    // expose non-user-visible baseline deltas that do not map to a global offset.
    // Memory: prefer public Pstates20 memory clocks vs VBIOS max clocks.
    // This reflects the currently active offset and avoids stale NVML/Pstates delta fields
    // surviving after another tool resets memory to default.
    read_nvidia_smi_max_clocks();
    int gpuOffsetkHz = uniform_curve_offset_khz();
    if (gpuOffsetkHz != 0 || g_app.pstateGpuOffsetkHz != 0) {
        if (gpuOffsetkHz == 0) gpuOffsetkHz = g_app.pstateGpuOffsetkHz;
        if (!g_app.gpuOffsetRangeKnown) {
            int gpuOffsetMHz = gpuOffsetkHz / 1000;
            g_app.gpuClockOffsetMinMHz = gpuOffsetMHz;
            g_app.gpuClockOffsetMaxMHz = gpuOffsetMHz;
        }
    }
    g_app.gpuClockOffsetkHz = gpuOffsetkHz;
    if (g_app.pstateMemMaxMHz > 0 && g_app.smiMemMaxMHz > 0) {
        int memOffsetkHz = ((int)g_app.pstateMemMaxMHz - (int)g_app.smiMemMaxMHz) * 1000;
        g_app.memClockOffsetkHz = memOffsetkHz;
        if (!g_app.memOffsetRangeKnown && memOffsetkHz != 0) {
            int memOffsetMHz = mem_display_mhz_from_driver_khz(memOffsetkHz);
            g_app.memClockOffsetMinMHz = memOffsetMHz;
            g_app.memClockOffsetMaxMHz = memOffsetMHz;
        }
    }
}
static bool nvapi_read_offsets() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->readSupported) return false;
    HeapBuffer buf(backend->controlBufferSize > 0 ? backend->controlBufferSize : 0x4000);
    if (!buf) return false;
    memset(buf, 0, backend->controlBufferSize);
    if (backend->controlBufferSize > 0x4000) return false;
    if (!nvapi_read_control_table(buf, backend->controlBufferSize)) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        int delta = 0;
        unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
        if (deltaOffset + sizeof(delta) > backend->controlBufferSize || !buf.read_at(deltaOffset, &delta, sizeof(delta))) {
            g_app.freqOffsets[i] = 0;
            continue;
        }
        g_app.freqOffsets[i] = delta;
    }
    return true;
}
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    unsigned char vfMaskByte = g_app.vfMask[pointIndex / 8];
    if (!(vfMaskByte & (1u << (pointIndex % 8)))) {
        debug_log("set_point: point %d not in vfMask, skipping\n", pointIndex);
        return false;
    }
    freqDelta_kHz = clamp_freq_delta_khz(freqDelta_kHz);
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;
    auto func = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!func) return false;
    HeapBuffer buf(backend->controlBufferSize > 0 ? backend->controlBufferSize : 0x4000);
    if (!buf) return false;
    memset(buf, 0, backend->controlBufferSize);
    if (backend->controlBufferSize > 0x4000) return false;
    if (!nvapi_read_control_table(buf, backend->controlBufferSize)) return false;
    memset(&buf[backend->controlMaskOffset], 0, 32);
    buf[backend->controlMaskOffset + pointIndex / 8] = (unsigned char)(1 << (pointIndex % 8));
    unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)pointIndex * backend->controlEntryStride + backend->controlEntryDeltaOffset;
    if (deltaOffset + sizeof(freqDelta_kHz) > backend->controlBufferSize || !buf.write_at(deltaOffset, &freqDelta_kHz, sizeof(freqDelta_kHz))) {
        return false;
    }
    char phase[128] = {};
    StringCchPrintfA(phase, ARRAY_COUNT(phase), "VF point write: point=%d delta=%d", pointIndex, freqDelta_kHz);
    set_last_apply_phase(phase);
    int ret = func(g_app.gpuHandle, buf);
    debug_log("set_point idx=%d delta=%d ret=%d\n", pointIndex, freqDelta_kHz, ret);
    return ret == 0;
}
// Pstates20 struct size and version for Blackwell
// NVML-based OC/PL functions
static bool nvml_read_power_limit() {
    if (!nvml_ensure_ready()) return false;
    if (!g_nvml_api.getPowerLimit || !g_nvml_api.getPowerDefaultLimit) return false;
    unsigned int cur = 0, def = 0;
    if (g_nvml_api.getPowerLimit(g_app.nvmlDevice, &cur) != NVML_SUCCESS) return false;
    if (g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &def) != NVML_SUCCESS) def = cur;
    g_app.powerLimitCurrentmW = (int)cur;
    g_app.powerLimitDefaultmW = def > 0 ? (int)def : (int)cur;
    g_app.powerLimitMinmW = 0;
    g_app.powerLimitMaxmW = 0;
    if (g_nvml_api.getPowerConstraints) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getPowerConstraints(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.powerLimitMinmW = (int)mn;
            g_app.powerLimitMaxmW = (int)mx;
        }
    }
    if (g_app.powerLimitDefaultmW > 0)
        g_app.powerLimitPct = (g_app.powerLimitCurrentmW * 100 + g_app.powerLimitDefaultmW / 2) / g_app.powerLimitDefaultmW;
    else
        g_app.powerLimitPct = 100;
    if (g_app.powerLimitPct < 0) g_app.powerLimitPct = 0;
    return true;
}
static bool nvapi_read_pstates() {
    // Read clock data from public NvAPI Pstates20.
    auto func = (NvApiFunc)nvapi_qi(0x6FF81213u);
    g_app.pstateGpuOffsetkHz = 0;
    g_app.pstateMemMaxMHz = 0;
    if (!func) return false;
    nvapiPerfPstates20Info_t info = {};
    info.version = NVAPI_PERF_PSTATES20_INFO_VER3;
    int ret = func(g_app.gpuHandle, &info);
    if (ret != 0) {
        info = {};
        info.version = NVAPI_PERF_PSTATES20_INFO_VER2;
        ret = func(g_app.gpuHandle, &info);
    }
    if (ret != 0) return false;
    unsigned int numPstates = info.numPstates;
    if (numPstates > NVAPI_MAX_GPU_PSTATE20_PSTATES) numPstates = NVAPI_MAX_GPU_PSTATE20_PSTATES;
    unsigned int numClocks = info.numClocks;
    if (numClocks > NVAPI_MAX_GPU_PSTATE20_CLOCKS) numClocks = NVAPI_MAX_GPU_PSTATE20_CLOCKS;
    bool curveRangeAnyFound = false;
    bool curveRangeP0Found = false;
    int curveRangeAnyMinkHz = 0;
    int curveRangeAnyMaxkHz = 0;
    int curveRangeP0MinkHz = 0;
    int curveRangeP0MaxkHz = 0;
    auto update_curve_range = [](bool* found, int* minOut, int* maxOut, int minValue, int maxValue) {
        if (!found || !minOut || !maxOut) return;
        if (!*found) {
            *minOut = minValue;
            *maxOut = maxValue;
            *found = true;
            return;
        }
        if (minValue < *minOut) *minOut = minValue;
        if (maxValue > *maxOut) *maxOut = maxValue;
    };
    for (unsigned int pi = 0; pi < numPstates; pi++) {
        const nvapiPstate20Entry_t* pstate = &info.pstates[pi];
        for (unsigned int ci = 0; ci < numClocks; ci++) {
            const nvapiPstate20ClockEntry_t* clock = &pstate->clocks[ci];
            unsigned int maxFreq_kHz = 0;
            if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE) {
                maxFreq_kHz = clock->data.single.freq_kHz;
            } else if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE) {
                maxFreq_kHz = clock->data.range.maxFreq_kHz;
            }
            if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS) {
                if (abs(clock->freqDelta_kHz.value) > abs(g_app.pstateGpuOffsetkHz)) {
                    g_app.pstateGpuOffsetkHz = clock->freqDelta_kHz.value;
                }
                if (clock->bIsEditable) {
                    if (pstate->pstateId == NVML_PSTATE_0) {
                        update_curve_range(&curveRangeP0Found, &curveRangeP0MinkHz, &curveRangeP0MaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    } else {
                        update_curve_range(&curveRangeAnyFound, &curveRangeAnyMinkHz, &curveRangeAnyMaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    }
                }
            } else if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY) {
                unsigned int mhz = maxFreq_kHz / 1000;
                if (mhz > g_app.pstateMemMaxMHz) g_app.pstateMemMaxMHz = mhz;
            }
        }
    }
    if (curveRangeP0Found) {
        set_curve_offset_range_khz(curveRangeP0MinkHz, curveRangeP0MaxkHz);
    } else if (curveRangeAnyFound) {
        set_curve_offset_range_khz(curveRangeAnyMinkHz, curveRangeAnyMaxkHz);
    }
    return true;
}
static bool nvapi_set_gpu_offset(int offsetkHz) {
    if (!vf_curve_global_gpu_offset_supported()) {
        if (g_app.gpuClockOffsetkHz == offsetkHz) return true;
        bool exact = false;
        char detail[128] = {};
        set_last_apply_phase("GPU offset NVML write");
        bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_GRAPHICS, offsetkHz / 1000, &exact, detail, sizeof(detail));
        if (!ok) return false;
        nvml_read_clock_offsets(detail, sizeof(detail));
        nvapi_read_pstates();
        detect_clock_offsets();
        nvapi_read_offsets();
        if (nvapi_read_curve()) rebuild_visible_map();
        return exact || g_app.gpuClockOffsetkHz == offsetkHz;
    }
    int currentGlobalkHz = uniform_curve_offset_khz();
    if (currentGlobalkHz == offsetkHz) return true;
    debug_log("set_gpu_offset current=%d target=%d\n", currentGlobalkHz, offsetkHz);
    int targetOffsets[VF_NUM_POINTS] = {};
    bool pointMask[VF_NUM_POINTS] = {};
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        targetOffsets[i] = clamp_freq_delta_khz(g_app.freqOffsets[i] - currentGlobalkHz + offsetkHz);
        pointMask[i] = true;
    }
    set_last_apply_phase("GPU offset VF curve write");
    bool exactOk = apply_curve_offsets_verified(targetOffsets, pointMask, 2);
    int uniformkHz = uniform_curve_offset_khz();
    detect_clock_offsets();
    bool functionalOk = (uniformkHz == offsetkHz) || (g_app.gpuClockOffsetkHz == offsetkHz);
    debug_log("set_gpu_offset result exact=%d uniform=%d detected=%d\n",
        exactOk ? 1 : 0, uniformkHz, g_app.gpuClockOffsetkHz);
    return exactOk || functionalOk;
}
static bool nvapi_set_mem_offset(int offsetkHz) {
    if (g_app.memClockOffsetkHz == offsetkHz) return true;
    int nvmlValueMHz = (offsetkHz / 1000) * 2;
    debug_log("nvapi_set_mem_offset: target_driver_kHz=%d nvml_value_MHz=%d current_driver_kHz=%d\n",
        offsetkHz, nvmlValueMHz, g_app.memClockOffsetkHz);
    bool exact = false;
    char detail[128] = {};
    set_last_apply_phase("Memory offset NVML write");
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_MEM, nvmlValueMHz, &exact, detail, sizeof(detail));
    if (!ok) {
        debug_log("nvapi_set_mem_offset: NVML rejected offset (nvml=%d MHz) detail=%s\n",
            nvmlValueMHz, detail[0] ? detail : "unknown");
        return false;
    }
    nvml_read_clock_offsets(detail, sizeof(detail));
    nvapi_read_pstates();
    detect_clock_offsets();
    bool verified = (g_app.memClockOffsetkHz == offsetkHz);
    debug_log("nvapi_set_mem_offset: apply ok, readback_driver_kHz=%d verified=%d\n",
        g_app.memClockOffsetkHz, verified ? 1 : 0);
    return verified;
}
static bool nvapi_set_power_limit(int pct) {
    if (pct < 50 || pct > 150) return false;
    if (g_app.powerLimitDefaultmW <= 0) return false;
    unsigned int targetmW = (unsigned int)(((long long)g_app.powerLimitDefaultmW * pct + 50) / 100);
    if (targetmW < 1) return false;
    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) return false;
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) return false;
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
static void rebuild_visible_map() {
    g_app.numVisible = 0;
    int populatedOrdinal = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        g_app.populatedOrdinal[i] = -1;
        if (g_app.curve[i].freq_kHz == 0) continue;
        g_app.populatedOrdinal[i] = populatedOrdinal++;
        // Keep the editable VF grid stable across applied offsets and curve edits.
        // Visibility should follow the baseline point position, not the current live target.
        unsigned int freq_mhz = (unsigned int)(curve_base_khz_for_point(i) / 1000);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (volt_mv >= MIN_VISIBLE_VOLT_mV && freq_mhz >= MIN_VISIBLE_FREQ_MHz) {
            g_app.visibleMap[g_app.numVisible++] = i;
        }
    }
}
static bool restore_locked_tail_from_curve_index_exact(int preferredCi) {
    if (preferredCi < 0 || preferredCi >= VF_NUM_POINTS) return false;
    if (g_app.numVisible < 2) return false;
    if (g_app.curve[preferredCi].freq_kHz == 0) return false;
    int preferredVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == preferredCi) {
            preferredVi = vi;
            break;
        }
    }
    if (preferredVi < 0 || preferredVi >= g_app.numVisible - 1) return false;
    unsigned int lockFreqkHz = g_app.curve[preferredCi].freq_kHz;
    bool hasTail = false;
    for (int j = preferredVi + 1; j < g_app.numVisible; j++) {
        int cj = g_app.visibleMap[j];
        if (g_app.curve[cj].freq_kHz == 0) return false;
        hasTail = true;
        if (g_app.curve[cj].freq_kHz != lockFreqkHz) return false;
    }
    if (!hasTail) return false;
    g_app.lockedVi = preferredVi;
    g_app.lockedCi = preferredCi;
    g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
    g_app.lockMode = LOCK_MODE_FLATTEN;
    g_app.guiLockTracksAnchor = true;
    return true;
}
static bool restore_locked_tail_from_curve_index_tolerant(int preferredCi, int minTailPoints) {
    if (preferredCi < 0 || preferredCi >= VF_NUM_POINTS) return false;
    if (g_app.numVisible < 2) return false;
    if (g_app.curve[preferredCi].freq_kHz == 0) return false;
    int preferredVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == preferredCi) {
            preferredVi = vi;
            break;
        }
    }
    if (preferredVi < 0 || preferredVi >= g_app.numVisible - 1) return false;
    static const int LOCK_TAIL_TOLERANCE_MHZ = 1;
    unsigned int anchorMHz = displayed_curve_mhz(g_app.curve[preferredCi].freq_kHz);
    unsigned int summedMHz = anchorMHz;
    int pointCount = 1;
    int tailPoints = 0;
    for (int j = preferredVi + 1; j < g_app.numVisible; j++) {
        int cj = g_app.visibleMap[j];
        if (g_app.curve[cj].freq_kHz == 0) return false;
        unsigned int pointMHz = displayed_curve_mhz(g_app.curve[cj].freq_kHz);
        if (abs((int)pointMHz - (int)anchorMHz) > LOCK_TAIL_TOLERANCE_MHZ) return false;
        summedMHz += pointMHz;
        pointCount++;
        tailPoints++;
    }
    if (tailPoints < minTailPoints) return false;
    g_app.lockedVi = preferredVi;
    g_app.lockedCi = preferredCi;
    g_app.lockedFreq = (summedMHz + (unsigned int)(pointCount / 2)) / (unsigned int)pointCount;
    g_app.lockMode = LOCK_MODE_FLATTEN;
    g_app.guiLockTracksAnchor = true;
    return true;
}
static void sync_applied_lock_state_from_curve() {
    if (!gui_state_dirty()) {
        g_app.appliedLockVi = g_app.lockedVi;
        g_app.appliedLockCi = g_app.lockedCi;
        g_app.appliedLockFreq = g_app.lockedFreq;
        // Keep the intent invariant: detection never creates pending user
        // intent, so appliedLockMode must track lockMode here.  A divergent
        // pair (lockMode != appliedLockMode) is reserved for real user intent
        // and blocks the snapshot lockMode sync (lock_mode_sync_allowed).
        g_app.appliedLockMode = g_app.lockMode;
    }
}

static void detect_locked_tail_from_curve() {
    int preferredCi = (g_app.lockedFreq > 0 && g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS)
        ? g_app.lockedCi
        : -1;
    if (!should_auto_detect_locked_tail_from_live_curve()) {
        if (preferredCi >= 0) debug_log("detect_locked_tail_from_curve: live lock detection suppressed; preserving intent ci=%d mhz=%u\n", g_app.lockedCi, g_app.lockedFreq);
        return;
    }
    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.lockMode = LOCK_MODE_NONE;
    g_app.guiLockTracksAnchor = true;
    sync_applied_lock_state_from_curve();
    if (g_app.numVisible < 2) return;
    if (preferredCi >= 0) {
        if (restore_locked_tail_from_curve_index_exact(preferredCi)) {
            sync_applied_lock_state_from_curve();
            return;
        }
        if (restore_locked_tail_from_curve_index_tolerant(preferredCi, 1)) {
            sync_applied_lock_state_from_curve();
            return;
        }
    }
    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int lockFreqkHz = g_app.curve[ci].freq_kHz;
        if (lockFreqkHz == 0) continue;
        bool hasTail = false;
        bool allSame = true;
        for (int j = vi + 1; j < g_app.numVisible; j++) {
            int cj = g_app.visibleMap[j];
            unsigned int freqkHz = g_app.curve[cj].freq_kHz;
            if (freqkHz == 0) {
                allSame = false;
                break;
            }
            hasTail = true;
            if (freqkHz != lockFreqkHz) {
                allSame = false;
                break;
            }
        }
        if (hasTail && allSame) {
            g_app.lockedVi = vi;
            g_app.lockedCi = ci;
            g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
            g_app.lockMode = LOCK_MODE_FLATTEN;
            sync_applied_lock_state_from_curve();
            return;
        }
    }
    // Some drivers report a flattened tail with tiny per-point kHz drift even though
    // the visible curve is effectively locked. Accept a long suffix that stays within
    // 1 MHz of the anchor so startup detection does not jump to a later voltage point.
    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        if (restore_locked_tail_from_curve_index_tolerant(ci, 3)) {
            sync_applied_lock_state_from_curve();
            return;
        }
    }
    // No lock detected - appliedLock already synced at function entry.
}

static bool read_live_curve_snapshot_settled(int attempts, DWORD delayMs, bool* lastOffsetsOkOut) {
    if (!g_app.isServiceProcess && g_app.usingBackgroundService) {
        char err[256] = {};
        ServiceResponse stateResponse = {};
        if (!service_client_get_ready_state(&stateResponse, 2000,
                "settled curve state", err, sizeof(err))) {
            debug_log("service snapshot failed: %s\n", err);
            if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
            return false;
        }
        apply_ready_service_envelope_to_app(&stateResponse);
        if (lastOffsetsOkOut) *lastOffsetsOkOut = true;
        return stateResponse.snapshot.loaded;
    }
    if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
    if (attempts < 1) attempts = 1;
    bool anyCurveOk = false;
    bool bestValid = false;
    bool bestOffsetsOk = false;
    int bestNumVisible = -1;
    int bestNumPopulated = -1;
    VFCurvePoint bestCurve[VF_NUM_POINTS] = {};
    int bestFreqOffsets[VF_NUM_POINTS] = {};
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0 && delayMs > 0) Sleep(delayMs);
        bool curveOk = nvapi_read_curve();
        bool offsetsOk = nvapi_read_offsets();
        if (!curveOk) continue;
        anyCurveOk = true;
        rebuild_visible_map();
        detect_locked_tail_from_curve();
        bool betterSnapshot = !bestValid
            || g_app.numVisible > bestNumVisible
            || (g_app.numVisible == bestNumVisible && g_app.numPopulated > bestNumPopulated)
            || (g_app.numVisible == bestNumVisible && g_app.numPopulated == bestNumPopulated && offsetsOk && !bestOffsetsOk);
        if (betterSnapshot) {
            memcpy(bestCurve, g_app.curve, sizeof(bestCurve));
            memcpy(bestFreqOffsets, g_app.freqOffsets, sizeof(bestFreqOffsets));
            bestNumVisible = g_app.numVisible;
            bestNumPopulated = g_app.numPopulated;
            bestOffsetsOk = offsetsOk;
            bestValid = true;
        }
    }
    if (!bestValid) {
        if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
        return anyCurveOk;
    }
    memcpy(g_app.curve, bestCurve, sizeof(g_app.curve));
    memcpy(g_app.freqOffsets, bestFreqOffsets, sizeof(g_app.freqOffsets));
    g_app.numPopulated = bestNumPopulated;
    g_app.loaded = true;
    rebuild_visible_map();
    detect_locked_tail_from_curve();
    debug_log("read_live_curve_snapshot_settled: selected visible=%d populated=%d offsetsOk=%d attempts=%d\n",
        bestNumVisible,
        bestNumPopulated,
        bestOffsetsOk ? 1 : 0,
        attempts);
    if (lastOffsetsOkOut) *lastOffsetsOkOut = bestOffsetsOk;
    return true;
}
