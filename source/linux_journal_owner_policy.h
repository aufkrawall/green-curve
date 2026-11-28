// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_journal_owner_policy.h -- whose GPU the daemon's saved journal
// (active.bin) belongs to, and whether a write to the GPU being served may
// proceed while it is on disk.
//
// The journal carries its card's identity (PCI identity, location role and an
// optional UUID). It is attributed to a live card only by the existing restore
// resolver, never by guessing. Until each GPU has its own runtime, only one
// card may hold Green Curve state, on disk as in memory: a write to card X is
// refused while the journal belongs to another card that is present. A journal
// whose card cannot be found is released only by an explicit Reset of the
// served card, and only when discovery is complete (so "not listed" really
// means "not there") and no other present card could own it, and never
// unfinished recovery.

#ifndef GREEN_CURVE_LINUX_JOURNAL_OWNER_POLICY_H
#define GREEN_CURVE_LINUX_JOURNAL_OWNER_POLICY_H

#include "linux_gpu_uuid.h"

enum LinuxJournalOwner {
    LINUX_JOURNAL_OWNER_NONE = 0,        // no journal on disk
    LINUX_JOURNAL_OWNER_SERVED = 1,      // resolves to the served card
    LINUX_JOURNAL_OWNER_OTHER = 2,       // resolves to another present card
    LINUX_JOURNAL_OWNER_GONE = 3,        // proven absent, or only the served card can be it
    LINUX_JOURNAL_OWNER_UNPROVEN = 4,    // ambiguous, contradicted or incomplete evidence
    LINUX_JOURNAL_OWNER_UNREADABLE = 5,  // several cards' journals, or unreadable
};

static inline const char* linux_journal_owner_name(LinuxJournalOwner owner) {
    switch (owner) {
        case LINUX_JOURNAL_OWNER_NONE: return "none";
        case LINUX_JOURNAL_OWNER_SERVED: return "served";
        case LINUX_JOURNAL_OWNER_OTHER: return "other";
        case LINUX_JOURNAL_OWNER_GONE: return "gone";
        case LINUX_JOURNAL_OWNER_UNPROVEN: return "unproven";
        default: return "unreadable";
    }
}

struct LinuxJournalEvidence {
    const GpuAdapterInfo* adapters;
    const LinuxGpuUuid* uuids;
    unsigned int count;
    unsigned int reportedCount;  // NVML's device count
    int servedIndex;             // -1: nothing served
};

// Discovery proves absence only when it listed every device NVML reported,
// each with PCI identity.
static inline bool linux_journal_discovery_complete(const LinuxJournalEvidence& e) {
    if (!e.adapters || e.count == 0 || e.count != e.reportedCount || e.count > MAX_GPU_ADAPTERS)
        return false;
    for (unsigned int i = 0; i < e.count; ++i)
        if (!e.adapters[i].valid || !e.adapters[i].pciInfoValid) return false;
    return true;
}

// Every device NVML reported was listed and has a UUID read in this session.
static inline bool linux_journal_every_uuid_read(const LinuxJournalEvidence& e) {
    if (!linux_journal_discovery_complete(e) || !e.uuids) return false;
    for (unsigned int i = 0; i < e.count; ++i)
        if (!linux_gpu_uuid_valid(&e.uuids[i])) return false;
    return true;
}

static inline bool linux_journal_uuid_unique(const LinuxJournalEvidence& e, unsigned int index) {
    for (unsigned int j = 0; j < e.count; ++j)
        if (j != index && linux_gpu_uuid_valid(&e.uuids[j]) &&
            strcmp(e.uuids[j].text, e.uuids[index].text) == 0) return false;
    return true;
}

static inline LinuxJournalOwner linux_journal_owner(const GpuAdapterInfo& target,
    GpuLocationRole role, const LinuxGpuUuid& uuid, const LinuxJournalEvidence& e,
    int* ownerIndex) {
    if (ownerIndex) *ownerIndex = -1;
    LinuxGpuUuidVerdict verdict = LINUX_GPU_UUID_FALLBACK;
    const LinuxGpuInitSelection selection = linux_gpu_restore_selection(
        &target, role, e.adapters, e.count, &uuid, e.uuids, &verdict);
    // On a host with several GPUs a journal that names its card's UUID is
    // proven only by a current read of that UUID (e.uuids, this NVML
    // session's), and only while every reported card was listed and read: a
    // location or a model cannot tell which card it is once cards can be
    // swapped, moved or not opened, and a UUID that looks unique among
    // partial reads may be a duplicate. A single GPU, and a journal written
    // without a UUID, keep the location rule as before; while discovery
    // skipped a device that is only the journal's exact location, never its
    // model followed to a twin.
    if (selection.writeResolved && selection.index >= 0 && linux_gpu_uuid_valid(&uuid) &&
        (e.count > 1 || e.reportedCount > 1) &&
        (verdict != LINUX_GPU_UUID_MATCHED || !linux_journal_every_uuid_read(e)))
        return LINUX_JOURNAL_OWNER_UNPROVEN;
    if (selection.writeResolved && selection.index >= 0 && verdict != LINUX_GPU_UUID_MATCHED) {
        if (!linux_journal_discovery_complete(e) &&
            !(linux_gpu_bdf_valid(&target) && linux_gpu_bdf_equal(&target, &e.adapters[selection.index])))
            return LINUX_JOURNAL_OWNER_UNPROVEN;
    }
    if (selection.writeResolved && selection.index >= 0) {
        if (ownerIndex) *ownerIndex = selection.index;
        return selection.index == e.servedIndex ? LINUX_JOURNAL_OWNER_SERVED
                                                : LINUX_JOURNAL_OWNER_OTHER;
    }
    if (!linux_journal_discovery_complete(e) || e.servedIndex < 0) return LINUX_JOURNAL_OWNER_UNPROVEN;
    // Not resolved. It is gone only if no other present card could be it: no
    // other card reports its UUID or shares its model.
    for (unsigned int i = 0; i < e.count; ++i) {
        if ((int)i == e.servedIndex) continue;
        const bool liveUuid = e.uuids && linux_gpu_uuid_valid(&e.uuids[i]);
        if (linux_gpu_uuid_valid(&uuid) && liveUuid && strcmp(uuid.text, e.uuids[i].text) == 0)
            return LINUX_JOURNAL_OWNER_UNPROVEN;
        // A card reporting its own UUID, unique on this host (every card read,
        // so no unread card may share it), that differs from the journal's is
        // provably another card, whatever its model.
        if (linux_gpu_uuid_valid(&uuid) && liveUuid && linux_journal_every_uuid_read(e) &&
            linux_journal_uuid_unique(e, i)) continue;
        if (!linux_gpu_model_known(&target) || linux_gpu_model_matches(&target, &e.adapters[i]))
            return LINUX_JOURNAL_OWNER_UNPROVEN;
    }
    // Either no present card is it, or only the served card can be (a card
    // whose recorded location no longer resolves). Both end the same way: an
    // explicit Reset of the served card may take the journal over.
    return LINUX_JOURNAL_OWNER_GONE;
}

enum LinuxJournalWrite {
    LINUX_JOURNAL_WRITE_APPLY = 0,      // explicit Apply
    LINUX_JOURNAL_WRITE_RESET = 1,      // explicit Reset
    LINUX_JOURNAL_WRITE_AUTOMATIC = 2,  // boot or resume restore
};

enum LinuxJournalDecision {
    LINUX_JOURNAL_ALLOW = 0,     // no journal, or the served card's own
    LINUX_JOURNAL_RELEASE = 1,   // explicit Reset removes a gone card's settled journal, nothing else
    LINUX_JOURNAL_REFUSE = 2,
};

// `settledIntent` is a plain committed journal (ACTIVE, no named graphics
// recovery). Another card's Reset never takes over unfinished recovery
// (PREPARED, UNCERTAIN, named controls). Releasing a settled journal is a
// step of its own that writes no hardware; the card's own Apply or Reset
// follows separately, so no single record has to hold two claims.
static inline LinuxJournalDecision linux_journal_write_decision(
    LinuxJournalOwner owner, LinuxJournalWrite write, bool settledIntent) {
    switch (owner) {
        case LINUX_JOURNAL_OWNER_NONE:
        case LINUX_JOURNAL_OWNER_SERVED:
            return LINUX_JOURNAL_ALLOW;
        case LINUX_JOURNAL_OWNER_GONE:
            return write == LINUX_JOURNAL_WRITE_RESET && settledIntent
                ? LINUX_JOURNAL_RELEASE : LINUX_JOURNAL_REFUSE;
        default:
            return LINUX_JOURNAL_REFUSE;
    }
}

#endif  // GREEN_CURVE_LINUX_JOURNAL_OWNER_POLICY_H
