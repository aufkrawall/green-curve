// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Cheap invalidation state for GUI profile-ownership synchronization.

struct AppliedProfileSyncCache {
    bool valid;
    bool authoritative;
    bool activeDesiredValid;
    ServiceProfileSource source;
    unsigned int sourceSlot;
    char configPath[MAX_PATH];
    bool configPresent;
    DWORD configSizeHigh;
    DWORD configSizeLow;
    FILETIME configLastWrite;
    DesiredSettings activeDesired;
};
static AppliedProfileSyncCache g_appliedProfileSyncCache = {};

static void applied_profile_config_stamp(const char* path, bool* presentOut,
    DWORD* sizeHighOut, DWORD* sizeLowOut, FILETIME* lastWriteOut) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    bool present = path && path[0] &&
        gc_GetFileAttributesExUtf8(path, GetFileExInfoStandard, &data) &&
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    *presentOut = present;
    *sizeHighOut = present ? data.nFileSizeHigh : 0;
    *sizeLowOut = present ? data.nFileSizeLow : 0;
    *lastWriteOut = present ? data.ftLastWriteTime : FILETIME{};
}

static bool applied_profile_sync_inputs_unchanged(
    const AppliedProfileSyncCache& current) {
    if (!g_appliedProfileSyncCache.valid) return false;
    const AppliedProfileSyncCache& cached = g_appliedProfileSyncCache;
    return cached.authoritative == current.authoritative &&
        cached.activeDesiredValid == current.activeDesiredValid &&
        cached.source == current.source && cached.sourceSlot == current.sourceSlot &&
        _stricmp(cached.configPath, current.configPath) == 0 &&
        cached.configPresent == current.configPresent &&
        cached.configSizeHigh == current.configSizeHigh &&
        cached.configSizeLow == current.configSizeLow &&
        CompareFileTime(&cached.configLastWrite, &current.configLastWrite) == 0 &&
        (!current.activeDesiredValid ||
         memcmp(&cached.activeDesired, &current.activeDesired,
             sizeof(current.activeDesired)) == 0);
}

static AppliedProfileSyncCache current_applied_profile_sync_inputs() {
    AppliedProfileSyncCache current = {};
    current.valid = true;
    current.authoritative = g_app.serviceSnapshotAuthoritative;
    current.activeDesiredValid = g_app.serviceActiveDesiredValid;
    current.source = g_app.serviceActiveProfileSource;
    current.sourceSlot = g_app.serviceActiveProfileSlot;
    StringCchCopyA(current.configPath, ARRAY_COUNT(current.configPath),
        g_app.configPath);
    applied_profile_config_stamp(current.configPath, &current.configPresent,
        &current.configSizeHigh, &current.configSizeLow,
        &current.configLastWrite);
    if (current.activeDesiredValid) {
        current.activeDesired = g_app.serviceActiveDesired;
    }
    return current;
}
