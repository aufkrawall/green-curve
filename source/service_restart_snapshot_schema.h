// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_RESTART_SNAPSHOT_SCHEMA_H
#define GREEN_CURVE_SERVICE_RESTART_SNAPSHOT_SCHEMA_H

// The controlled-restart snapshot's on-disk shape, and the frozen layout of the
// generation before it.  Split out of main_service_persist.cpp, which is on its
// size ratchet; the reader/writer stay there.
//
// Recovery after a GPU device reconnect, TDR, or driver upgrade restarts the
// service PROCESS.  Before exiting it snapshots the active desired OC/fan
// profile here, and the relaunched process may re-apply it only when it
// presents the nonce issued to the protected restart helper.
//
// SERVICE_ACTIVE_DESIRED_VERSION sat at 5 from 0.19.2 onward while this payload
// silently grew several times, so "version 5" on disk names no single layout --
// only the `size` word in the file header does.  The loader therefore
// dispatches on size and reads version for meaning, exactly like the two Linux
// daemon records; see desired_settings_schema.h.  The former LEGACY_VERSION 4
// acceptance is gone with that: it demanded the CURRENT payload size, so no
// genuine v4 file could ever have satisfied it, and v4's layout is not
// identifiable after the fact.

#define SERVICE_ACTIVE_DESIRED_MAGIC   0x47434144u /* 'GCAD' */
#define SERVICE_ACTIVE_DESIRED_VERSION 6u
#define SERVICE_ACTIVE_DESIRED_SCHEMA1_VERSION 5u

struct ServiceRestartReapplySnapshot {
    DesiredSettings desired;
    GpuAdapterInfo targetGpu;
    DWORD activeProfileSource;
    DWORD activeProfileSlot;
    DWORD reserved[2];
};

static_assert(sizeof(ServiceRestartReapplySnapshot) == 1160,
              "ServiceRestartReapplySnapshot changed size: freeze the outgoing "
              "layout as ServiceRestartReapplySnapshotSchema<N>, teach the "
              "loader its size, and bump SERVICE_ACTIVE_DESIRED_VERSION "
              "(see desired_settings_schema.h)");

// FROZEN: the payload 0.25.2 wrote under version 5.  Never edit.
struct ServiceRestartReapplySnapshotSchema1 {
    DesiredSettingsSchema1 desired;
    GpuAdapterInfo targetGpu;
    DWORD activeProfileSource;
    DWORD activeProfileSlot;
    DWORD reserved[2];
};
static_assert(sizeof(ServiceRestartReapplySnapshotSchema1) == 1032,
              "ServiceRestartReapplySnapshotSchema1 is a FROZEN on-disk layout");
static_assert(sizeof(ServiceRestartReapplySnapshot) !=
                  sizeof(ServiceRestartReapplySnapshotSchema1),
              "restart snapshot layouts must be distinguishable by size");

static inline void service_restart_snapshot_widen_schema1(
    ServiceRestartReapplySnapshot* out,
    const ServiceRestartReapplySnapshotSchema1* in) {
    if (!out || !in) return;
    *out = {};
    desired_settings_widen_from_schema1(&out->desired, &in->desired);
    out->targetGpu = in->targetGpu;
    out->activeProfileSource = in->activeProfileSource;
    out->activeProfileSlot = in->activeProfileSlot;
    // `reserved` stays zero: schema 1 never carried anything in it, and a
    // widened record must not invent bytes the source file did not have.
}

#endif
