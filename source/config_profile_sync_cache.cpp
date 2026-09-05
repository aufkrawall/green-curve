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
    // Which VF points are populated, as a bitmask.
    //
    // The ownership read is drift-free (PROFILE_READ_FOR_OWNERSHIP), but it is
    // still TOPOLOGY-scoped: a `curve_semantics=base_plus_gpu_offset` profile is
    // turned back into absolute MHz through is_gpu_offset_excluded_low_point(),
    // which counts populated points.  Populated-ness does not move with
    // temperature the way frequency does, but it does move when the GPU or its
    // VF table changes -- and a cache key that omits an input the decision reads
    // is exactly how the previous defect stayed invisible until an unrelated
    // event flushed it out.  So it is part of the key.
    gc_u64 populatedMask[VF_NUM_POINTS / 64];
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
        memcmp(cached.populatedMask, current.populatedMask,
            sizeof(current.populatedMask)) == 0 &&
        (!current.activeDesiredValid ||
         desired_settings_equal(&cached.activeDesired, &current.activeDesired));
}

// VF_NUM_POINTS must stay a whole number of 64-bit words for the mask above.
static_assert(VF_NUM_POINTS % 64 == 0,
    "the populated-point mask assumes VF_NUM_POINTS is a multiple of 64");

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
    // g_app.loaded gates whether the curve array holds anything at all; an
    // all-zero mask is the honest answer while the GPU is reconnecting.
    if (g_app.loaded) {
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            if (g_app.curve[i].freq_kHz == 0) continue;
            current.populatedMask[i / 64] |= (gc_u64)1 << (i % 64);
        }
    }
    return current;
}
