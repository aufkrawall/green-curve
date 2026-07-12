// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Event-driven user-mode display-stack readiness. Configuration Manager can
// publish DEVICEINSTANCESTARTED before NVIDIA's user-mode API is usable. DXGI's
// adapter-set notification occurs at the user-mode adapter layer and gives the
// lifecycle worker a later real readiness edge without a timer, sleep, or
// hardware polling loop.

static HMODULE g_serviceDxgiModule = nullptr;
static IDXGIFactory7* g_serviceDxgiFactory = nullptr;
static HANDLE g_serviceDxgiAdaptersChangedEvent = nullptr;
static DWORD g_serviceDxgiAdaptersChangedCookie = 0;

static void service_stop_dxgi_adapter_readiness();

static bool service_start_dxgi_adapter_readiness(char* err, size_t errSize) {
    if (g_serviceDxgiAdaptersChangedEvent) return true;
    g_serviceDxgiModule = load_system_library_a("dxgi.dll");
    if (!g_serviceDxgiModule) {
        set_message(err, errSize, "Could not load system DXGI (error %lu)",
            GetLastError());
        return false;
    }
    typedef HRESULT (WINAPI *CreateDxgiFactory1Fn)(REFIID, void**);
    CreateDxgiFactory1Fn createFactory =
        reinterpret_cast<CreateDxgiFactory1Fn>(
            GetProcAddress(g_serviceDxgiModule, "CreateDXGIFactory1"));
    IDXGIFactory1* factory1 = nullptr;
    HRESULT result = createFactory
        ? createFactory(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&factory1))
        : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (FAILED(result) || !factory1) {
        set_message(err, errSize, "CreateDXGIFactory1 failed (hr=0x%08lX)",
            (unsigned long)result);
        service_stop_dxgi_adapter_readiness();
        return false;
    }
    result = factory1->QueryInterface(__uuidof(IDXGIFactory7),
        reinterpret_cast<void**>(&g_serviceDxgiFactory));
    factory1->Release();
    if (FAILED(result) || !g_serviceDxgiFactory) {
        set_message(err, errSize,
            "IDXGIFactory7 adapter notifications unavailable (hr=0x%08lX)",
            (unsigned long)result);
        service_stop_dxgi_adapter_readiness();
        return false;
    }
    g_serviceDxgiAdaptersChangedEvent =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_serviceDxgiAdaptersChangedEvent) {
        set_message(err, errSize,
            "Could not create DXGI adapter readiness event (error %lu)",
            GetLastError());
        service_stop_dxgi_adapter_readiness();
        return false;
    }
    result = g_serviceDxgiFactory->RegisterAdaptersChangedEvent(
        g_serviceDxgiAdaptersChangedEvent,
        &g_serviceDxgiAdaptersChangedCookie);
    if (FAILED(result) || g_serviceDxgiAdaptersChangedCookie == 0) {
        set_message(err, errSize,
            "DXGI adapter readiness registration failed (hr=0x%08lX)",
            (unsigned long)result);
        service_stop_dxgi_adapter_readiness();
        return false;
    }
    debug_log("lifecycle DXGI readiness: registered adapter-set change event cookie=%lu\n",
        (unsigned long)g_serviceDxgiAdaptersChangedCookie);
    return true;
}

static void service_stop_dxgi_adapter_readiness() {
    if (g_serviceDxgiFactory && g_serviceDxgiAdaptersChangedCookie != 0) {
        g_serviceDxgiFactory->UnregisterAdaptersChangedEvent(
            g_serviceDxgiAdaptersChangedCookie);
    }
    g_serviceDxgiAdaptersChangedCookie = 0;
    if (g_serviceDxgiAdaptersChangedEvent) {
        CloseHandle(g_serviceDxgiAdaptersChangedEvent);
        g_serviceDxgiAdaptersChangedEvent = nullptr;
    }
    if (g_serviceDxgiFactory) {
        g_serviceDxgiFactory->Release();
        g_serviceDxgiFactory = nullptr;
    }
    if (g_serviceDxgiModule) {
        FreeLibrary(g_serviceDxgiModule);
        g_serviceDxgiModule = nullptr;
    }
}
