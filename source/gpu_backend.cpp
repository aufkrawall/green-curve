// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "gpu_backend_apply.cpp"
static bool apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
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
        bool ok = service_client_apply_desired(desired, g_pendingOperationSource[0] ? g_pendingOperationSource : "client apply", interactive, result, resultSize, &snapshot);
        if (snapshot.initialized || snapshot.loaded) {
            apply_service_snapshot_to_app(&snapshot);
            if (desired && desired->hasLock && desired->lockCi >= 0 && desired->lockCi < VF_NUM_POINTS && snapshot.curve[desired->lockCi].freq_kHz > 0) {
                g_app.lockedCi = desired->lockCi;
                g_app.lockedFreq = displayed_curve_mhz(snapshot.curve[desired->lockCi].freq_kHz);
                g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
                for (int vi = 0; vi < g_app.numVisible; vi++) {
                    if (g_app.visibleMap[vi] == desired->lockCi) {
                        g_app.lockedVi = vi;
                        break;
                    }
                }
                debug_log("service apply client lock sync: ci=%d liveMHz=%u trackAnchor=%d\n",
                    g_app.lockedCi,
                    g_app.lockedFreq,
                    g_app.guiLockTracksAnchor ? 1 : 0);
            }
            if (g_app.hMainWnd) {
                populate_global_controls();
                if (g_app.loaded) populate_edits();
                invalidate_main_window();
            }
        }
        return ok;
    }
    return apply_desired_settings_service(desired, interactive, result, resultSize);
}
// ============================================================================
// NvAPI Interface
// ============================================================================
static void* nvapi_qi(unsigned int id) {
    typedef void* (*qi_func)(unsigned int);
    static qi_func qi = nullptr;
    if (!qi) {
        g_app.hNvApi = load_system_library_a("nvapi64.dll");
        if (!g_app.hNvApi) {
            g_app.hNvApi = load_system_library_a("nvapi.dll");
        }
        if (!g_app.hNvApi) return nullptr;
        qi = (qi_func)GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!qi) return nullptr;
    }
    return qi(id);
}
static bool nvapi_init() {
    typedef int (*init_t)();
    auto init = (init_t)nvapi_qi(NVAPI_INIT_ID);
    if (!init) return false;
    return init() == 0;
}

static bool gpu_adapter_has_same_pci_identity(const GpuAdapterInfo* a, const GpuAdapterInfo* b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    if (!a->pciInfoValid || !b->pciInfoValid) return false;
    return a->deviceId == b->deviceId &&
        a->subSystemId == b->subSystemId &&
        a->pciRevisionId == b->pciRevisionId &&
        a->extDeviceId == b->extDeviceId;
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

static void load_selected_gpu_from_config() {
    if (g_app.selectedGpuExplicit) return;
    int selected = get_config_int(g_app.configPath, "gpu", "selected_index", 0);
    if (selected < 0) selected = 0;
    if (selected >= MAX_GPU_ADAPTERS) selected = MAX_GPU_ADAPTERS - 1;
    g_app.selectedGpuIndex = (unsigned int)selected;
    g_app.selectedNvmlIndex = (unsigned int)selected;
    g_app.selectedGpuExplicit = true;
}

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
    auto enumGpu = (enum_t)nvapi_qi(NVAPI_ENUM_GPU_ID);
    if (!enumGpu) return false;
    int count = 0;
    GPU_HANDLE handles[64] = {};
    int ret = enumGpu(handles, &count);
    if (ret != 0 || count < 1) return false;
    load_selected_gpu_from_config();
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
        adapter->family = g_app.gpuFamily;
        adapter->vfReadSupported = g_app.vfBackend && g_app.vfBackend->readSupported;
        adapter->vfWriteSupported = g_app.vfBackend && g_app.vfBackend->writeSupported;
        adapter->vfBestGuess = vf_backend_is_best_guess(g_app.vfBackend);
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
    unsigned char buf[0x4000] = {};
    if (backend->statusBufferSize > sizeof(buf)) return false;
    {
        const unsigned int version = (backend->statusVersion << 16) | backend->statusBufferSize;
        memcpy(&buf[0], &version, sizeof(version));
    }
    if (backend->statusMaskOffset + sizeof(mask) > backend->statusBufferSize) return false;
    memcpy(&buf[backend->statusMaskOffset], mask, sizeof(mask));
    if (backend->statusNumClocksOffset + sizeof(numClocks) <= backend->statusBufferSize) {
        memcpy(&buf[backend->statusNumClocksOffset], &numClocks, sizeof(numClocks));
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
        memcpy(&freq, &buf[entryOffset], sizeof(freq));
        memcpy(&volt, &buf[entryOffset + 4], sizeof(volt));
        g_app.curve[i].freq_kHz = freq;
        g_app.curve[i].volt_uV = volt;
        if (freq > 0) g_app.numPopulated++;
    }
    g_app.loaded = true;
    return true;
}
static void read_nvidia_smi_max_clocks() {
    // Read nvidia-smi VBIOS default max clocks once, cache in AppData
    if (g_app.smiClocksRead) return;
    g_app.smiClocksRead = true;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};
    WCHAR exePath[MAX_PATH] = {};
    if (!find_trusted_nvidia_smi_path_w(exePath, ARRAY_COUNT(exePath))) {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        debug_log("nvidia-smi clock read skipped: trusted executable not found\n");
        return;
    }
    WCHAR cmd[MAX_PATH + 64] = {};
    StringCchPrintfW(cmd, ARRAY_COUNT(cmd), L"\"%ls\" -q -d CLOCK", exePath);
    if (CreateProcessW(exePath, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        char buf[4096] = {};
        DWORD totalRead = 0;
        bool timedOut = false;
        ULONGLONG startTickMs = GetTickCount64();
        while (totalRead < sizeof(buf) - 1) {
            DWORD available = 0;
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD toRead = (DWORD)nvmin((int)available, (int)(sizeof(buf) - 1 - totalRead));
                DWORD n = 0;
                if (!ReadFile(hRead, buf + totalRead, toRead, &n, nullptr) || n == 0) break;
                totalRead += n;
                continue;
            }
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 25);
            if (waitResult == WAIT_OBJECT_0) break;
            if (GetTickCount64() - startTickMs >= 5000) {
                timedOut = true;
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
        if (timedOut) {
            debug_log("nvidia-smi clock read timed out and was terminated\n");
        }
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        bool inMaxSection = false;
        char* line = buf;
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
                if ((vp = strstr(line, "Memory")) && (vp = strchr(vp, ':')))
                    g_app.smiMemMaxMHz = (unsigned int)atoi(vp + 1);
            }
            line = nextLine;
        }
    } else {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);
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
    unsigned char buf[0x4000] = {};
    if (backend->controlBufferSize > sizeof(buf)) return false;
    if (!nvapi_read_control_table(buf, sizeof(buf))) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        int delta = 0;
        unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
        if (deltaOffset + sizeof(delta) > backend->controlBufferSize) {
            g_app.freqOffsets[i] = 0;
            continue;
        }
        memcpy(&delta, &buf[deltaOffset], sizeof(delta));
        g_app.freqOffsets[i] = delta;
    }
    return true;
}
static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    freqDelta_kHz = clamp_freq_delta_khz(freqDelta_kHz);
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;
    auto func = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!func) return false;
    unsigned char buf[0x4000] = {};
    if (backend->controlBufferSize > sizeof(buf)) return false;
    if (!nvapi_read_control_table(buf, sizeof(buf))) return false;
    memset(&buf[backend->controlMaskOffset], 0, 32);
    buf[backend->controlMaskOffset + pointIndex / 8] = (unsigned char)(1 << (pointIndex % 8));
    unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)pointIndex * backend->controlEntryStride + backend->controlEntryDeltaOffset;
    if (deltaOffset + sizeof(freqDelta_kHz) > backend->controlBufferSize) return false;
    memcpy(&buf[deltaOffset], &freqDelta_kHz, sizeof(freqDelta_kHz));
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
    int watts = (g_app.powerLimitDefaultmW * pct + 50000) / 100000;
    if (watts < 1) return false;
    unsigned int targetmW = (unsigned int)watts * 1000u;
    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) return false;
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) return false;
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
    WCHAR cmdLine[MAX_PATH + 64] = {};
    StringCchPrintfW(cmdLine, ARRAY_COUNT(cmdLine), L"\"%ls\" -pl %d", exePath, watts);
    set_last_apply_phase("Power limit nvidia-smi write");
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("Power limit via nvidia-smi failed to launch (error %lu)\n", GetLastError());
        return false;
    }
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 5000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        debug_log("Power limit via nvidia-smi timed out and was terminated\n");
        return false;
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode == 0) {
        nvml_read_power_limit();
    }
    else debug_log("Power limit via nvidia-smi failed with exit code %lu\n", exitCode);
    return exitCode == 0;
}
static void rebuild_visible_map() {
    g_app.numVisible = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
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
    g_app.guiLockTracksAnchor = true;
    return true;
}
static void detect_locked_tail_from_curve() {
    int preferredCi = (g_app.lockedFreq > 0 && g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS)
        ? g_app.lockedCi
        : -1;
    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;
    g_app.guiLockTracksAnchor = true;
    if (g_app.numVisible < 2) return;
    if (!should_auto_detect_locked_tail_from_live_curve()) return;
    if (preferredCi >= 0) {
        if (restore_locked_tail_from_curve_index_exact(preferredCi)) return;
        if (restore_locked_tail_from_curve_index_tolerant(preferredCi, 1)) return;
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
            return;
        }
    }
    // Some drivers report a flattened tail with tiny per-point kHz drift even though
    // the visible curve is effectively locked. Accept a long suffix that stays within
    // 1 MHz of the anchor so startup detection does not jump to a later voltage point.
    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        if (restore_locked_tail_from_curve_index_tolerant(ci, 3)) {
            return;
        }
    }
}
static bool read_live_curve_snapshot_settled(int attempts, DWORD delayMs, bool* lastOffsetsOkOut) {
    if (!g_app.isServiceProcess && g_app.usingBackgroundService) {
        char err[256] = {};
        ServiceSnapshot snapshot = {};
        if (!service_client_get_snapshot(&snapshot, err, sizeof(err))) {
            debug_log("service snapshot failed: %s\n", err);
            if (lastOffsetsOkOut) *lastOffsetsOkOut = false;
            return false;
        }
        apply_service_snapshot_to_app(&snapshot);
        DesiredSettings activeDesired = {};
        if (service_client_get_active_desired(&activeDesired, nullptr, err, sizeof(err))) {
            apply_service_desired_to_gui(&activeDesired);
        }
        if (lastOffsetsOkOut) *lastOffsetsOkOut = true;
        return snapshot.loaded;
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
