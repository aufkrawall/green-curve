// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "app_shared.h"

void trim_ascii(char* s) {
    if (!s) return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;

    if (start > 0 && end > start) {
        memmove(s, s + start, (size_t)(end - start));
    }

    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;

    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || *end != 0) return false;
    if (v < -2147483647L - 1L || v > 2147483647L) return false;

    *out = (int)v;
    return true;
}

bool parse_cli_point_arg_w(const WCHAR* arg, int* pointIndexOut) {
    if (pointIndexOut) *pointIndexOut = -1;
    if (!arg || wcsncmp(arg, L"--point", 7) != 0) return false;

    const WCHAR* digits = arg + 7;
    if (!*digits) return false;

    int value = 0;
    for (const WCHAR* p = digits; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        value = value * 10 + (int)(*p - L'0');
        if (value >= VF_NUM_POINTS) return false;
    }

    if (pointIndexOut) *pointIndexOut = value;
    return true;
}

bool gpu_family_uses_best_guess_backend(GpuFamily family) {
    return family == GPU_FAMILY_UNKNOWN ||
        family == GPU_FAMILY_PASCAL ||
        family == GPU_FAMILY_TURING ||
        family == GPU_FAMILY_AMPERE;
}

void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;

    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;

    char buf[64] = {};
    if (text) StringCchCopyA(buf, ARRAY_COUNT(buf), text);
    trim_ascii(buf);
    if (buf[0] == 0 || streqi_ascii(buf, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (value < 0 || value > 100) return false;

    *isAuto = false;
    *pct = value;
    return true;
}

static HANDLE g_configInterprocessMutex = nullptr;

static HANDLE ensure_config_interprocess_mutex() {
    if (!g_configInterprocessMutex) {
        g_configInterprocessMutex = CreateMutexA(nullptr, FALSE, "Local\\GreenCurveConfigMutex");
    }
    return g_configInterprocessMutex;
}

bool enter_config_storage_lock(HANDLE* acquiredMutex) {
    if (acquiredMutex) *acquiredMutex = nullptr;
    EnterCriticalSection(&g_configLock);

    HANDLE mutex = ensure_config_interprocess_mutex();
    if (!mutex) return true;

    DWORD waitResult = WaitForSingleObject(mutex, 5000);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        if (acquiredMutex) *acquiredMutex = mutex;
        return true;
    }

    LeaveCriticalSection(&g_configLock);
    return false;
}

void leave_config_storage_lock(HANDLE acquiredMutex) {
    if (acquiredMutex) ReleaseMutex(acquiredMutex);
    LeaveCriticalSection(&g_configLock);
}

bool config_section_has_keys(const char* path, const char* section) {
    if (!path || !section) return false;

    char keys[256] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    DWORD n = GetPrivateProfileStringA(section, nullptr, "", keys, ARRAY_COUNT(keys), path);
    leave_config_storage_lock(configMutex);
    return n > 0 && keys[0] != 0;
}

int get_config_int(const char* path, const char* section, const char* key, int defaultVal) {
    if (!path || !section || !key) return defaultVal;

    char buf[32] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return defaultVal;
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    leave_config_storage_lock(configMutex);
    trim_ascii(buf);
    if (!buf[0]) return defaultVal;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return defaultVal;
    return value;
}

bool set_config_int(const char* path, const char* section, const char* key, int value) {
    if (!path || !section || !key) return false;

    char buf[32] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", value);
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    bool ok = WritePrivateProfileStringA(section, key, buf, path) != FALSE;
    leave_config_storage_lock(configMutex);
    if (ok) invalidate_tray_profile_cache();
    return ok;
}
