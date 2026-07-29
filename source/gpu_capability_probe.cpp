// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Read-only control-surface probe for the selected adapter (Windows).
//
// Family detection tells us which private NVAPI struct layout to use; it does
// not tell us which control domains this particular board exposes.  On every
// discrete GeForce board we have validated the answer was "all of them", so
// nothing ever asked.  Integrated Grace/Blackwell parts (RTX Spark / GB10) can
// report a Blackwell architecture while lacking a dedicated memory controller,
// a board-level TGP, and a driver-owned fan.
//
// This probe only READS.  It never writes GPU state, and per the conservatism
// in gpu_capability_classify() it downgrades a domain only on positive evidence
// from the driver — never merely because an optional entry point is missing on
// an older driver.  A GPU where everything works therefore probes complete and
// every dependent path stays inert.

// Included by main_shell.cpp AFTER main_runtime_nvml.cpp, so the NVML shard's
// static nvml_ensure_ready() and the debug_log helpers are already in scope.
// hardware_initialize() (main_state_sync.cpp) is included earlier and reaches
// gpu_probe_control_surface() through the forward declaration in main.cpp.

// Ask DXGI for the selected adapter's dedicated/shared memory split.  DXGI is
// already a dependency (the service uses IDXGIFactory7 for adapter readiness)
// and is the one interface that states the split directly; NVML exposes a
// total, not a topology.
//
// Every failure path returns UNKNOWN.  That matters for the LocalSystem service,
// which runs in session 0 where DXGI may be unavailable: a working discrete GPU
// must never be downgraded just because the query did not answer.
static gc_u32 probe_memory_topology_via_dxgi() {
    if (!g_app.gpuPciInfoValid || !g_app.gpuDeviceId) {
        debug_log("gpu capability probe: memory topology unknown (no PCI identity to match)\n");
        return GPU_MEMORY_TOPOLOGY_UNKNOWN;
    }
    HMODULE dxgi = load_system_library_a("dxgi.dll");
    if (!dxgi) {
        debug_log("gpu capability probe: memory topology unknown (dxgi.dll unavailable)\n");
        return GPU_MEMORY_TOPOLOGY_UNKNOWN;
    }
    typedef HRESULT (WINAPI *CreateDxgiFactory1Fn)(REFIID, void**);
    auto createFactory = reinterpret_cast<CreateDxgiFactory1Fn>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = createFactory
        ? createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))
        : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (FAILED(hr) || !factory) {
        debug_log("gpu capability probe: memory topology unknown"
                  " (CreateDXGIFactory1 hr=0x%08lX)\n", (unsigned long)hr);
        FreeLibrary(dxgi);
        return GPU_MEMORY_TOPOLOGY_UNKNOWN;
    }

    // NvAPI reports a PACKED identifier (device in the high word, vendor 0x10DE
    // in the low word — e.g. 0x2F0410DE for an RTX 5070), while DXGI reports the
    // bare 16-bit device id (0x2F04).  Comparing the two directly never matches,
    // which silently left every topology UNKNOWN.  Normalize the same way the
    // Linux binding policy does: find 0x10DE in either word.
    unsigned int nvapiId = g_app.gpuDeviceId;
    // Low word holding the vendor means the device is in the high word; every
    // other shape (vendor in the high word, or a bare 16-bit device id) already
    // carries the device in the low word.
    unsigned int wantDeviceId = (nvapiId & 0xFFFFu) == 0x10DEu
        ? (nvapiId >> 16) : (nvapiId & 0xFFFFu);

    gc_u32 topology = GPU_MEMORY_TOPOLOGY_UNKNOWN;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc))) {
            // Match the adapter Green Curve actually selected; a machine can
            // hold several NVIDIA GPUs plus a basic-render device.
            if (desc.VendorId == 0x10DEu && desc.DeviceId == wantDeviceId) {
                topology = gpu_memory_topology_from_sizes(
                    (unsigned long long)desc.DedicatedVideoMemory,
                    (unsigned long long)desc.SharedSystemMemory);
                debug_log("gpu capability probe: DXGI adapter %04X:%04X"
                          " dedicatedVRAM=%llu MiB sharedSystem=%llu MiB -> %s\n",
                          desc.VendorId, desc.DeviceId,
                          (unsigned long long)desc.DedicatedVideoMemory / (1024ull * 1024ull),
                          (unsigned long long)desc.SharedSystemMemory / (1024ull * 1024ull),
                          gpu_memory_topology_name(topology));
                adapter->Release();
                break;
            }
        }
        if (adapter) adapter->Release();
        adapter = nullptr;
    }
    if (topology == GPU_MEMORY_TOPOLOGY_UNKNOWN) {
        debug_log("gpu capability probe: memory topology unknown"
                  " (no DXGI adapter matched deviceId=0x%04X, from nvapi 0x%08X)\n",
                  wantDeviceId, nvapiId);
    }
    factory->Release();
    FreeLibrary(dxgi);
    return topology;
}

static void probe_log_domain(gc_u32 mask, gc_u32 capability, const char* how) {
    int index = gpu_capability_index_for_mask(mask);
    debug_log("gpu capability probe: %-14s = %-20s (%s)\n",
              gpu_capability_domain_name(index),
              gpu_capability_name(capability),
              how ? how : "");
}

void gpu_probe_control_surface() {
    GpuCapabilityProbe probe = {};

    if (!nvml_ensure_ready() || !g_app.nvmlDevice) {
        // Leave every domain UNPROBED: without NVML we have no evidence, and
        // absence of evidence must not subtract a capability.
        g_app.gpuCapability = probe;
        debug_log("gpu capability probe: NVML not ready, leaving all domains unprobed"
                  " (reports full surface by design)\n");
        return;
    }

    nvmlDevice_t dev = g_app.nvmlDevice;

    // --- Clock offsets (GPU and memory) -----------------------------------
    // Probed through nvml_get_offset_range(), the SAME resolver the apply path
    // uses, so the probe can never disagree with what an apply would actually
    // do.  This is load-bearing: an earlier version called only the legacy
    // nvmlDeviceGet{Gpc,Mem}ClkMinMaxVfOffset entry points and reported BOTH
    // offset domains as "refused by driver" on a working RTX 5070 — current
    // drivers answer NOT_SUPPORTED there and serve the modern P-state-scoped
    // getClockOffsets instead.  That false positive would have raised the new
    // reduced-surface warning on healthy x64 installs.
    struct {
        unsigned int domain;
        gc_u32 mask;
        const char* label;
    } offsetDomains[] = {
        { NVML_CLOCK_GRAPHICS, SERVICE_MUTATION_DOMAIN_GPU_OFFSET, "graphics" },
        { NVML_CLOCK_MEM, SERVICE_MUTATION_DOMAIN_MEM_OFFSET, "memory" },
    };
    for (size_t i = 0; i < ARRAY_COUNT(offsetDomains); ++i) {
        GpuDomainObservation obs = {};
        int mn = 0, mx = 0, cur = 0;
        char why[160] = {};
        // Either the modern or the legacy entry point counts as "present";
        // only when neither exists do we stay UNPROBED.
        obs.entryPointPresent = g_nvml_api.getClockOffsets != nullptr ||
            (offsetDomains[i].domain == NVML_CLOCK_GRAPHICS
                ? g_nvml_api.getGpcClkMinMaxVfOffset != nullptr
                : g_nvml_api.getMemClkMinMaxVfOffset != nullptr);
        if (obs.entryPointPresent) {
            obs.readSucceeded = nvml_get_offset_range(offsetDomains[i].domain,
                                                      &mn, &mx, &cur,
                                                      why, sizeof(why));
        }
        gc_u32 cap = gpu_capability_classify(&obs);
        gpu_capability_set(&probe, offsetDomains[i].mask, cap);
        probe_log_domain(offsetDomains[i].mask, cap,
                         obs.entryPointPresent ? "nvml_get_offset_range"
                                               : "no offset entry point (older driver)");
        if (obs.entryPointPresent) {
            debug_log("gpu capability probe: %s offset range %d..%d MHz current=%d%s%s\n",
                      offsetDomains[i].label, mn, mx, cur,
                      why[0] ? " detail=" : "", why[0] ? why : "");
        }
    }

    // --- Power limit ------------------------------------------------------
    {
        GpuDomainObservation obs = {};
        unsigned int mn = 0, mx = 0;
        obs.entryPointPresent = g_nvml_api.getPowerConstraints != nullptr;
        if (obs.entryPointPresent) {
            obs.readSucceeded =
                g_nvml_api.getPowerConstraints(dev, &mn, &mx) == NVML_SUCCESS;
        }
        // Deliberately NOT inferring "absent" from an empty min==max window:
        // fixed-TGP laptop boards that Green Curve supports today report
        // exactly that, and would start raising the limited-surface warning.
        // Only an outright refused read downgrades this domain; the measured
        // window is logged below so a real SoC report is still diagnosable.
        gc_u32 cap = gpu_capability_classify(&obs);
        gpu_capability_set(&probe, SERVICE_MUTATION_DOMAIN_POWER, cap);
        probe_log_domain(SERVICE_MUTATION_DOMAIN_POWER, cap,
                         obs.entryPointPresent ? "nvmlDeviceGetPowerManagementLimitConstraints"
                                               : "entry point absent (older driver)");
        if (obs.entryPointPresent && obs.readSucceeded) {
            debug_log("gpu capability probe: power constraints min=%u mW max=%u mW\n", mn, mx);
        }
    }

    // --- Fan --------------------------------------------------------------
    // Zero fans is positive evidence: the platform EC owns cooling, not the
    // NVIDIA driver.
    {
        GpuDomainObservation obs = {};
        unsigned int fans = 0;
        obs.entryPointPresent = g_nvml_api.getNumFans != nullptr;
        if (obs.entryPointPresent) {
            obs.readSucceeded = g_nvml_api.getNumFans(dev, &fans) == NVML_SUCCESS;
            if (obs.readSucceeded && fans == 0) obs.hardwareAbsent = 1;
        }
        gc_u32 cap = gpu_capability_classify(&obs);
        gpu_capability_set(&probe, SERVICE_MUTATION_DOMAIN_FAN, cap);
        probe_log_domain(SERVICE_MUTATION_DOMAIN_FAN, cap, "nvmlDeviceGetNumFans");
        if (obs.entryPointPresent && obs.readSucceeded) {
            debug_log("gpu capability probe: driver reports %u fan(s)\n", fans);
        }
    }

    // --- VF curve ---------------------------------------------------------
    // The backend's own read is the probe: if the selected spec read the curve,
    // the private surface answered.  A populated curve is the evidence.
    {
        GpuDomainObservation obs = {};
        obs.entryPointPresent = g_app.vfBackend && g_app.vfBackend->readSupported;
        // A non-empty editability mask is the evidence that the private
        // pstates20 read actually returned a curve for this adapter.
        bool anyPoint = false;
        for (size_t i = 0; i < ARRAY_COUNT(g_app.vfMask); ++i) {
            if (g_app.vfMask[i]) { anyPoint = true; break; }
        }
        obs.readSucceeded = obs.entryPointPresent && anyPoint;
        gc_u32 cap = gpu_capability_classify(&obs);
        gpu_capability_set(&probe, SERVICE_MUTATION_DOMAIN_VF_CURVE, cap);
        probe_log_domain(SERVICE_MUTATION_DOMAIN_VF_CURVE, cap,
                         "private NVAPI pstates20 read");
    }

    // --- Clock lock -------------------------------------------------------
    // Cannot be read back without writing, so presence of the entry point is
    // all we may conclude; a missing one stays UNPROBED rather than absent.
    {
        GpuDomainObservation obs = {};
        obs.entryPointPresent = g_nvml_api.setGpuLockedClocks != nullptr;
        obs.readSucceeded = obs.entryPointPresent;
        gc_u32 cap = gpu_capability_classify(&obs);
        gpu_capability_set(&probe, SERVICE_MUTATION_DOMAIN_LOCK, cap);
        probe_log_domain(SERVICE_MUTATION_DOMAIN_LOCK, cap,
                         "nvmlDeviceSetGpuLockedClocks presence only (write-only API)");
    }

    // --- Memory topology --------------------------------------------------
    // Read from DXGI, which reports the dedicated/shared split directly.  Still
    // deliberately NOT inferred from the absent memory-offset domain: an older
    // driver or a refused read would otherwise masquerade as a topology fact.
    // Any failure here leaves UNKNOWN, which keeps the unified-memory
    // confirmation dormant rather than guessing.
    probe.memoryTopology = probe_memory_topology_via_dxgi();

    g_app.gpuCapability = probe;

    gc_u32 missing = gpu_capability_missing_domains(&probe);
    gc_u32 surface = gpu_capability_surface_class(&probe);
    debug_log("gpu capability probe: family=%d backend=%s surface=%s missingMask=0x%02X"
              " memoryTopology=%s\n",
              (int)g_app.gpuFamily,
              g_app.vfBackend && g_app.vfBackend->name ? g_app.vfBackend->name : "<none>",
              gpu_control_surface_class_name(surface),
              (unsigned)missing,
              gpu_memory_topology_name(probe.memoryTopology));
    if (missing != 0) {
        for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
            gc_u32 mask = gpu_capability_mask_for_index(i);
            if (missing & mask) {
                debug_log("gpu capability probe: MISSING domain %s (%s)\n",
                          gpu_capability_domain_name(i),
                          gpu_capability_name(gpu_capability_get(&probe, mask)));
            }
        }
    }
}
