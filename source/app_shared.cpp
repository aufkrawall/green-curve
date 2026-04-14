#include "app_shared.h"

int g_dpi = 96;
float g_scale = 1.0f;
CRITICAL_SECTION g_configLock = {};

AppData g_app = {};
NvmlApi g_nvml_api = {};
HMODULE g_nvml = nullptr;
bool g_debug_logging = false;

int nvmin(int a, int b) {
    return a < b ? a : b;
}

int nvmax(int a, int b) {
    return a > b ? a : b;
}

int dp(int px) {
    return (int)((float)px * g_scale);
}

void init_dpi() {
    InitializeCriticalSection(&g_configLock);

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForSystem_t)();
        auto pGetDpiForSystem = (GetDpiForSystem_t)GetProcAddress(user32, "GetDpiForSystem");
        if (pGetDpiForSystem) {
            g_dpi = (int)pGetDpiForSystem();
            g_scale = (float)g_dpi / 96.0f;
            return;
        }
    }

    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HANDLE, int, UINT*, UINT*);
        auto pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(shcore, "GetDpiForMonitor");
        if (pGetDpiForMonitor) {
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (pGetDpiForMonitor(nullptr, 0, &dpiX, &dpiY) == 0) {
                g_dpi = (int)dpiX;
            }
        }
        FreeLibrary(shcore);
    }

    if (g_dpi == 96) {
        HDC hdc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
    }

    g_scale = (float)g_dpi / 96.0f;
}