// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// NVAPI runtime loading and the QueryInterface entry point.  Split out of
// gpu_backend.cpp (at its size ratchet) when arm64 module selection was added;
// the preference order itself is the pure table in nvapi_module_policy.h.
//
// Included by main.cpp immediately before gpu_backend.cpp.

// Module-level cache so close_nvapi() can invalidate it across calls.
// A driver recovery never attempts to reload NvAPI in the stale process; the
// nonce-bound helper starts a fresh service process instead.
static void* (*g_nvapiQi)(unsigned int) = nullptr;

// Module preference order lives in nvapi_module_policy.h: on arm64 the native
// image is nvapia64.dll, and the nvapi64.dll/nvapi.dll in the same driver
// package are the x64/x86 emulation copies a native arm64 process cannot load.
static void* nvapi_qi(unsigned int id) {
    if (!g_nvapiQi) {
        if (!g_app.hNvApi) {
            nvapi_module_first(nvapi_module_host_arch(), [](const char* name) {
                g_app.hNvApi = load_system_library_a(name);
                debug_log("nvapi_qi: load %s -> %s\n", name,
                          g_app.hNvApi ? "loaded" : "not present");
                return g_app.hNvApi != nullptr;
            });
        }
        if (!g_app.hNvApi) return nullptr;
        g_nvapiQi = (void* (*)(unsigned int))GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!g_nvapiQi) return nullptr;
    }
    return g_nvapiQi(id);
}
