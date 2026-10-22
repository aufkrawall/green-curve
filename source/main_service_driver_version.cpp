// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "module_version_win32.h"

// NVIDIA driver image versions: the mapped (in-process) image, the image on
// disk, and the arrival-time comparison between them.  Also read by the
// driver-recovery proof (main_service_recovery_clock.cpp).

// Read a fixed-file-info version (MS + LS DWORDs) from a file on disk.
// Returns true and fills *outMs/*outLs on success, false on any error
// (file missing, no version resource, allocation failure).  Used by the
// on-disk driver version check below.
static bool service_get_file_version(const WCHAR* path, DWORD* outMs, DWORD* outLs) {
    if (!path || !outMs || !outLs) return false;
    DWORD handle = 0;
    DWORD infoSize = GetFileVersionInfoSizeW(path, &handle);
    if (infoSize == 0) return false;
    HeapBuffer buf((size_t)infoSize);
    if (!buf) return false;
    if (!GetFileVersionInfoW(path, handle, infoSize, buf)) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoLen = 0;
    if (!VerQueryValueW((LPVOID)(unsigned char*)buf, L"\\", (LPVOID*)&info, &infoLen)) return false;
    if (!info || infoLen < sizeof(VS_FIXEDFILEINFO)) return false;
    *outMs = info->dwFileVersionMS;
    *outLs = info->dwFileVersionLS;
    return true;
}

// Read the version of an already-loaded module from its mapped image
// (module_version_win32.h).  Never by GetModuleFileNameW + file read: after a
// driver update replaced the file at that path, the path names the NEW image
// while this process still runs the old one, so both sides of the in-process
// vs on-disk comparison below would read the new file.
static bool service_get_module_version(HMODULE mod, DWORD* outMs, DWORD* outLs) {
    return gc_module_mapped_version(mod, outMs, outLs);
}

// Build the on-disk path for nvml.dll.  Prefers the NVSMI copy
// (%ProgramFiles%\NVIDIA Corporation\NVSMI\nvml.dll) because the driver
// installer updates both copies in lockstep.  Falls back to system32.
static bool service_get_nvml_disk_path(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryW(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return false;
    if (FAILED(StringCchPrintfW(out, outCount, L"%ls\\nvml.dll", systemDir))) return false;
    return true;
}

// The System32 NvAPI image the loader would map (nvapi_module_first(), the
// same walk nvapi_qi() takes): nvapi64.dll on x64, nvapia64.dll on ARM64.
// Comparing any other image would weigh the native driver's recorded version
// against an emulation copy that versions on its own.
static bool service_get_nvapi_disk_path(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryW(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return false;
    const char* image = nvapi_module_first(nvapi_module_host_arch(), [&](const char* name) {
        if (FAILED(StringCchPrintfW(out, outCount, L"%ls\\%hs", systemDir, name))) return false;
        const DWORD attributes = GetFileAttributesW(out);
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    });
    if (!image) {
        out[0] = 0;
        debug_log("service_get_nvapi_disk_path: no NvAPI image for this architecture in System32\n");
        return false;
    }
    return true;
}

// Detect whether the on-disk nvml.dll / NvAPI image differ from the version
// currently mapped into the process.  After a driver upgrade, the in-process
// copy is the old version and the on-disk file is the new one.  Diagnostic
// only (service_check_disk_version_on_device_arrival): recovery always
// restarts the process, because an in-process reload returns the old image.
static bool service_nvml_disk_version_changed() {
    if (!g_nvml) return false;
    DWORD inMs = 0, inLs = 0;
    if (!service_get_module_version(g_nvml, &inMs, &inLs)) {
        debug_log("service_nvml_disk_version_changed: in-process version unavailable\n");
        return false;
    }
    WCHAR diskPath[MAX_PATH] = {};
    if (!service_get_nvml_disk_path(diskPath, ARRAY_COUNT(diskPath))) return false;
    DWORD onMs = 0, onLs = 0;
    if (!service_get_file_version(diskPath, &onMs, &onLs)) {
        debug_log("service_nvml_disk_version_changed: on-disk version unavailable for %ls\n", diskPath);
        return false;
    }
    if (inMs == onMs && inLs == onLs) return false;
    debug_log("service_nvml_disk_version_changed: in-process %u.%u.%u.%u differs from on-disk '%ls' %u.%u.%u.%u\n",
        (unsigned)(inMs >> 16), (unsigned)(inMs & 0xFFFFu),
        (unsigned)(inLs >> 16), (unsigned)(inLs & 0xFFFFu),
        diskPath,
        (unsigned)(onMs >> 16), (unsigned)(onMs & 0xFFFFu),
        (unsigned)(onLs >> 16), (unsigned)(onLs & 0xFFFFu));
    return true;
}

static bool service_nvapi_disk_version_changed() {
    if (!g_app.hNvApi) return false;
    DWORD inMs = 0, inLs = 0;
    if (!service_get_module_version(g_app.hNvApi, &inMs, &inLs)) {
        debug_log("service_nvapi_disk_version_changed: in-process version unavailable\n");
        return false;
    }
    WCHAR diskPath[MAX_PATH] = {};
    if (!service_get_nvapi_disk_path(diskPath, ARRAY_COUNT(diskPath))) return false;
    DWORD onMs = 0, onLs = 0;
    if (!service_get_file_version(diskPath, &onMs, &onLs)) {
        debug_log("service_nvapi_disk_version_changed: on-disk version unavailable for %ls\n", diskPath);
        return false;
    }
    if (inMs == onMs && inLs == onLs) return false;
    debug_log("service_nvapi_disk_version_changed: in-process %u.%u.%u.%u differs from on-disk '%ls' %u.%u.%u.%u\n",
        (unsigned)(inMs >> 16), (unsigned)(inMs & 0xFFFFu),
        (unsigned)(inLs >> 16), (unsigned)(inLs & 0xFFFFu),
        diskPath,
        (unsigned)(onMs >> 16), (unsigned)(onMs & 0xFFFFu),
        (unsigned)(onLs >> 16), (unsigned)(onLs & 0xFFFFu));
    return true;
}

// Log a single line per DLL when the on-disk version differs from the
// in-process module, on a selected-GPU arrival (lifecycle worker), to
// correlate the device restart with a driver upgrade.  Diagnostic only: the
// recovery decision does not depend on it, and a fresh process picks up the
// new files either way.  Returns whether an upgrade was seen.
static bool service_check_disk_version_on_device_arrival() {
    bool nvmlChanged = service_nvml_disk_version_changed();
    bool nvapiChanged = service_nvapi_disk_version_changed();
    if (nvmlChanged || nvapiChanged) {
        debug_log("service_check_disk_version_on_device_arrival: driver upgrade detected "
            "(nvmlChanged=%d nvapiChanged=%d) — recovery will pick up new files\n",
            nvmlChanged ? 1 : 0, nvapiChanged ? 1 : 0);
    } else {
        debug_log("service_check_disk_version_on_device_arrival: on-disk driver matches in-process\n");
    }
    return nvmlChanged || nvapiChanged;
}
