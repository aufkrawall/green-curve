// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by main_service_persist.cpp; controlled-restart record loading.

static bool service_load_restart_reapply_snapshot(DesiredSettings* out, GpuAdapterInfo* targetOut,
    ServiceProfileSource* profileSourceOut = nullptr, unsigned int* profileSlotOut = nullptr) {
    if (!out) return false;
    if (targetOut) memset(targetOut, 0, sizeof(*targetOut));
    if (profileSourceOut) *profileSourceOut = SERVICE_PROFILE_SOURCE_NONE;
    if (profileSlotOut) *profileSlotOut = 0;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return false;
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD magic = 0, version = 0, size = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &version, sizeof(version), &read, nullptr) && read == sizeof(version);
    ok = ok && ReadFile(h, &size, sizeof(size), &read, nullptr) && read == sizeof(size);
    // The header's `size` word is the layout discriminator; `version` then says
    // what the fields mean within that layout.  A schema-1 payload is what a
    // 0.25.2 service wrote, and widening it is what keeps a controlled restart
    // across an upgrade from silently dropping the active overclock.
    bool current = version == SERVICE_ACTIVE_DESIRED_VERSION &&
        size == (DWORD)sizeof(ServiceRestartReapplySnapshot);
    bool schema3 = version == SERVICE_ACTIVE_DESIRED_SCHEMA3_VERSION &&
        size == (DWORD)sizeof(ServiceRestartReapplySnapshotSchema3);
    bool schema2 = version == SERVICE_ACTIVE_DESIRED_SCHEMA2_VERSION &&
        size == (DWORD)sizeof(ServiceRestartReapplySnapshotSchema2);
    bool schema1 = version == SERVICE_ACTIVE_DESIRED_SCHEMA1_VERSION &&
        size == (DWORD)sizeof(ServiceRestartReapplySnapshotSchema1);
    if (ok && (magic != SERVICE_ACTIVE_DESIRED_MAGIC || !(current || schema3 || schema2 || schema1))) {
        debug_log("restart reapply load: header mismatch magic=%08lX ver=%lu size=%lu (expected v%u/%lu or v%u/%lu or v%u/%lu or v%u/%lu); clearing old snapshot\n",
            (unsigned long)magic, (unsigned long)version, (unsigned long)size,
            (unsigned)SERVICE_ACTIVE_DESIRED_VERSION,
            (unsigned long)sizeof(ServiceRestartReapplySnapshot),
            (unsigned)SERVICE_ACTIVE_DESIRED_SCHEMA3_VERSION,
            (unsigned long)sizeof(ServiceRestartReapplySnapshotSchema3),
            (unsigned)SERVICE_ACTIVE_DESIRED_SCHEMA2_VERSION,
            (unsigned long)sizeof(ServiceRestartReapplySnapshotSchema2),
            (unsigned)SERVICE_ACTIVE_DESIRED_SCHEMA1_VERSION,
            (unsigned long)sizeof(ServiceRestartReapplySnapshotSchema1));
        ok = false;
    } else if (ok && schema1) {
        debug_log("restart reapply load: widening v%u schema-1 snapshot (%lu bytes) to v%u\n",
            (unsigned)SERVICE_ACTIVE_DESIRED_SCHEMA1_VERSION,
            (unsigned long)size, (unsigned)SERVICE_ACTIVE_DESIRED_VERSION);
    }
    ServiceRestartReapplySnapshot payload = {};
    if (ok) ok = service_restart_snapshot_read_payload(version, size,
        [&](void* dst, size_t count) {
            return ReadFile(h, dst, (DWORD)count, &read, nullptr) && read == count;
        }, &payload);
    if (schema3)
        debug_log("restart reapply load: schema-3 snapshot widened (ok=%d)\n", ok ? 1 : 0);
    if (schema2)
        debug_log("restart reapply load: schema-2 snapshot widened (ok=%d)\n", ok ? 1 : 0);
    BYTE trailing = 0;
    DWORD trailingRead = 0;
    if (ok) ok = ReadFile(h, &trailing, sizeof(trailing), &trailingRead, nullptr) && trailingRead == 0;
    CloseHandle(h);
    if (!ok) {
        debug_log("restart reapply load: read failed\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    // The file lives in the admin-only SYSTEM-profile dir, so this is not a
    // user trust boundary — but a torn/corrupt write would feed raw bytes
    // straight into the apply path.  Clamp every field with the same
    // validator used at the IPC boundary (also keeps lockMode in enum range).
    DesiredSettings originalDesired = payload.desired;
    GpuAdapterInfo originalTarget = payload.targetGpu;
    validate_desired_settings_for_ipc(&payload.desired);
    validate_gpu_adapter_info_for_ipc(&payload.targetGpu);
    if (!service_desired_bool_fields_valid(&payload.desired) ||
        !desired_settings_equal(&originalDesired, &payload.desired) ||
        !gpu_adapter_info_equal(&originalTarget, &payload.targetGpu)) {
        debug_log("restart reapply load: payload fields required sanitization; rejecting corrupt snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    if (!payload.targetGpu.valid) {
        debug_log("restart reapply load: missing target GPU identity; clearing snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    ServiceProfileSource profileSource = (ServiceProfileSource)payload.activeProfileSource;
    bool slotted = profileSource == SERVICE_PROFILE_SOURCE_USER_SLOT ||
        profileSource == SERVICE_PROFILE_SOURCE_SHARED_SLOT ||
        profileSource == SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
    if (payload.activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC ||
        (slotted && (payload.activeProfileSlot < 1 || payload.activeProfileSlot > 5)) ||
        (!slotted && payload.activeProfileSlot != 0) ||
        payload.reserved[0] != 0 || payload.reserved[1] != 0) {
        debug_log("restart reapply load: invalid active profile metadata; rejecting snapshot\n");
        gc_DeleteFileUtf8(path);
        return false;
    }
    *out = payload.desired;
    if (targetOut) *targetOut = payload.targetGpu;
    if (profileSourceOut) *profileSourceOut = profileSource;
    if (profileSlotOut) *profileSlotOut = payload.activeProfileSlot;
    return true;
}
