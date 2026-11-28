// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp after the daemon state store. The saved
// journal (active.bin) as seen from the GPU about to be written: whose card it
// belongs to (linux_journal_owner_policy.h), whether this write may proceed,
// and its exact bytes, pinned so a failed write restores them.
#pragma once
#include "linux_journal_owner_policy.h"

struct LinuxJournalPin {
    LinuxJournalOwner owner;
    LinuxJournalDecision decision;
    int ownerIndex;
    bool present;                   // one journal was on disk before the write
    bool lost;                      // the loader just refused and removed it
    LinuxDaemonStateRecord record;  // its exact contents
};

static LinuxJournalEvidence daemon_journal_evidence(LinuxGpuController& ctl) {
    LinuxJournalEvidence evidence = {};
    linux_backend_expire_session_uuids(&ctl.gpu);
    evidence.adapters = ctl.gpu.adapters;
    evidence.uuids = ctl.gpu.adapterUuids;
    evidence.count = ctl.gpu.adapterCount;
    evidence.reportedCount = ctl.gpu.adapterReportedCount;
    evidence.servedIndex = ctl.gpu.writeIdentityResolved &&
        ctl.gpu.selectedAdapterIndex < ctl.gpu.adapterCount
        ? (int)ctl.gpu.selectedAdapterIndex : -1;
    return evidence;
}

// Whether the intent in memory names adapters[i] by UUID (its PCI identity is
// the caller's check): that card's own read now is the intent's UUID; with
// several GPUs a missing read is no proof, while a single GPU, or an intent
// without a UUID, keeps the location rule. Other cards' reads do not matter:
// this asks which card a handle addresses, not whether a stored record is
// unique on the host (linux_journal_owner does that for records on disk).
static bool daemon_intent_uuid_names(const LinuxGpuController& ctl, unsigned int i) {
    if (!linux_gpu_uuid_valid(&ctl.activeTargetUuid)) return true;
    // A UUID another listed card also reads proves neither card.
    for (unsigned int j = 0; j < ctl.gpu.adapterCount && j < MAX_GPU_ADAPTERS; ++j)
        if (j != i && linux_gpu_uuid_valid(&ctl.gpu.adapterUuids[j]) &&
            strcmp(ctl.gpu.adapterUuids[j].text, ctl.activeTargetUuid.text) == 0) return false;
    if (i < ctl.gpu.adapterCount && i < MAX_GPU_ADAPTERS && linux_gpu_uuid_valid(&ctl.gpu.adapterUuids[i]))
        return strcmp(ctl.gpu.adapterUuids[i].text, ctl.activeTargetUuid.text) == 0;
    return ctl.gpu.adapterCount <= 1 && ctl.gpu.adapterReportedCount <= 1;
}

// The intent's UUID is what adapters[i] reads now (both known), and no other
// listed card reads it too (a duplicate proves neither): the only proof that
// lets an intent follow its card to another PCI location.
static bool daemon_intent_uuid_read_on(const LinuxGpuController& ctl, unsigned int i) {
    return linux_gpu_uuid_valid(&ctl.activeTargetUuid) && i < ctl.gpu.adapterCount && i < MAX_GPU_ADAPTERS &&
        linux_gpu_uuid_valid(&ctl.gpu.adapterUuids[i]) && daemon_intent_uuid_names(ctl, i);
}

// The intent in memory belongs to the served card: the binding keeps its write
// identity and the card's UUID read in this session is the intent's. A card
// now at its old location is no proof. The fan worker, the snapshot's fan
// state and the adapter summary follow only this.
static bool daemon_active_intent_on_served(LinuxGpuController& ctl) {
    linux_backend_expire_session_uuids(&ctl.gpu);
    return ctl.hasActiveDesired && ctl.gpu.writeIdentityResolved &&
        daemon_intent_uuid_names(ctl, ctl.gpu.selectedAdapterIndex);
}

// Where a stored identity (a journal, a fan marker, the startup binding) is
// proven on this inventory, by the journal rule with the run's UUID evidence;
// -1 when it is not.
static int daemon_stored_gpu_index(LinuxGpuController& ctl, const GpuAdapterInfo& target,
                                   GpuLocationRole role, const LinuxGpuUuid& uuid) {
    int index = -1;
    const LinuxJournalOwner owner = linux_journal_owner(target, role, uuid, daemon_journal_evidence(ctl), &index);
    return owner == LINUX_JOURNAL_OWNER_SERVED || owner == LINUX_JOURNAL_OWNER_OTHER ? index : -1;
}

static const char* daemon_journal_write_name(LinuxJournalWrite write) {
    return write == LINUX_JOURNAL_WRITE_APPLY ? "apply"
        : write == LINUX_JOURNAL_WRITE_RESET ? "reset" : "automatic";
}

// Until each GPU has its own runtime, one card holds Green Curve state, on
// disk as in memory. Called after the target is selected and before anything
// is written; refuses with the remedy in `err`.
static bool daemon_journal_admit(LinuxGpuController& ctl, LinuxJournalWrite write,
                                 LinuxJournalPin* pin, char* err, size_t errSize) {
    *pin = LinuxJournalPin{};
    pin->ownerIndex = -1;
    if (ctl.journalHoldActive) {
        char holdErr[256] = {};
        if (!linux_daemon_state_store(GC_DAEMON_STATE_FILE, &ctl.journalHold, holdErr, sizeof(holdErr))) {
            gc_snprintf(err, errSize, "Saved Green Curve state could not be made durable (%s); no GPU is "
                "written until it can be stored again", holdErr[0] ? holdErr : "unknown error");
            dlog("daemon journal: %s refused; held journal still not durable: %s\n",
                 daemon_journal_write_name(write), holdErr[0] ? holdErr : "unknown error");
            return false;
        }
        ctl.journalHoldActive = false;
        dlog("daemon journal: held journal stored again; admission continues against it\n");
    }
    char loadErr[256] = {};
    bool migrated = false;
    const LinuxDaemonStateLoadResult load = linux_daemon_state_load(
        GC_DAEMON_STATE_FILE, &pin->record, loadErr, sizeof(loadErr), &migrated);
    const LinuxJournalEvidence evidence = daemon_journal_evidence(ctl);
    if (load == LINUX_DAEMON_STATE_LOADED) {
        pin->present = true;
        pin->owner = linux_journal_owner(pin->record.targetGpu,
            (GpuLocationRole)pin->record.targetLocationRole, pin->record.targetUuid,
            evidence, &pin->ownerIndex);
    } else {
        // A missing journal holds no claim; several GPUs' journals or a read
        // failure prove nothing. One the loader refused and removed holds no
        // claim an explicit write could violate, but an unattended write must
        // not proceed past evidence it just lost (boot treats it as recovery).
        pin->lost = load == LINUX_DAEMON_STATE_INVALID_REMOVED ||
            load == LINUX_DAEMON_STATE_LEGACY_REMOVED;
        pin->owner = load == LINUX_DAEMON_STATE_MULTIPLE || load == LINUX_DAEMON_STATE_IO_ERROR ||
            (pin->lost && write == LINUX_JOURNAL_WRITE_AUTOMATIC)
            ? LINUX_JOURNAL_OWNER_UNREADABLE : LINUX_JOURNAL_OWNER_NONE;
    }
    // A UUID-bound record is written only where this card could clear it again:
    // a first record, or one replacing this card's record that had no UUID,
    // must prove this card by the rule its Reset will use (with several GPUs,
    // every card read).
    const LinuxGpuUuid* own = linux_backend_selected_uuid(&ctl.gpu);
    const bool bindsUuidFirst = pin->owner == LINUX_JOURNAL_OWNER_NONE ||
        (pin->owner == LINUX_JOURNAL_OWNER_SERVED && !linux_gpu_uuid_valid(&pin->record.targetUuid));
    if (bindsUuidFirst && write != LINUX_JOURNAL_WRITE_RESET && evidence.servedIndex >= 0 &&
        own && linux_gpu_uuid_valid(own) && linux_journal_owner(ctl.gpu.selectedGpu, GPU_LOCATION_ROLE_UNRECORDED,
            *own, evidence, nullptr) != LINUX_JOURNAL_OWNER_SERVED)
        pin->owner = LINUX_JOURNAL_OWNER_UNPROVEN;
    const bool settledIntent = pin->present && pin->record.state == LINUX_DAEMON_RECORD_ACTIVE &&
        pin->record.graphicsControlSurfaces == 0;
    pin->decision = linux_journal_write_decision(pin->owner, write, settledIntent);
    const bool resetReleases = linux_journal_write_decision(pin->owner, LINUX_JOURNAL_WRITE_RESET,
        settledIntent) == LINUX_JOURNAL_RELEASE;
    const GpuAdapterInfo& stored = pin->record.targetGpu;
    dlog("daemon journal: %s owner=%s decision=%s load=%d state=%u named=%u stored=%04x:%02x:%02x.%u "
         "ownerIndex=%d served=%d adapters=%u reported=%u%s%s\n", daemon_journal_write_name(write),
         linux_journal_owner_name(pin->owner),
         pin->decision == LINUX_JOURNAL_ALLOW ? "allow"
             : pin->decision == LINUX_JOURNAL_RELEASE ? "release" : "refuse",
         (int)load, pin->present ? pin->record.state : 0u,
         pin->present ? pin->record.graphicsControlSurfaces : 0u,
         stored.pciDomain, stored.pciBus, stored.pciDevice, stored.pciFunction,
         pin->ownerIndex, evidence.servedIndex, evidence.count, evidence.reportedCount,
         loadErr[0] ? " detail=" : "", loadErr);
    if (pin->decision != LINUX_JOURNAL_REFUSE) return true;
    switch (pin->owner) {
        case LINUX_JOURNAL_OWNER_OTHER:
            gc_snprintf(err, errSize, "GPU %04x:%02x:%02x.%u holds saved Green Curve state; "
                "select it and Reset it before changing this GPU",
                ctl.gpu.adapters[pin->ownerIndex].pciDomain, ctl.gpu.adapters[pin->ownerIndex].pciBus,
                ctl.gpu.adapters[pin->ownerIndex].pciDevice, ctl.gpu.adapters[pin->ownerIndex].pciFunction);
            break;
        case LINUX_JOURNAL_OWNER_GONE:
            gc_strlcpy(err, errSize, resetReleases
                ? "Saved Green Curve state belongs to a GPU that is not present; Reset this GPU to release it"
                : "Unfinished Green Curve recovery belongs to a GPU that is not present; reconnect it and Reset it");
            break;
        case LINUX_JOURNAL_OWNER_UNPROVEN:
            gc_strlcpy(err, errSize, !pin->present
                ? "This GPU cannot be identified well enough to save its settings (with several GPUs, every "
                  "GPU's UUID must be readable and this one's unique)"
                : "Saved Green Curve state names a GPU that cannot be identified "
                  "(its UUID is not readable now, it is ambiguous, or not every GPU was discovered); "
                  "reconnect it or restore its PCI slot, then Reset it");
            break;
        default:
            gc_strlcpy(err, errSize, "Saved Green Curve state holds several GPUs' records or cannot be read; "
                "it is kept unchanged and no GPU is written");
            break;
    }
    return false;
}

// Releasing a missing card's settled journal is its own durable step before
// this card's Reset: every crash point leaves one consistent journal (the old
// one, none, or this Reset's own PREPARED record). False, with the reply
// filled, when the release could not complete; a journal that could not be
// put back holds every write (journalHoldActive) until it can be stored again.
static bool daemon_journal_release_for_reset(LinuxGpuController& ctl, const LinuxJournalPin& journal,
                                             ServiceResponse* resp) {
    char releaseErr[256] = {};
    if (linux_daemon_state_remove(GC_DAEMON_STATE_FILE, releaseErr, sizeof(releaseErr))) {
        dlog("daemon journal: Reset released the settled journal of a GPU that is not present; "
             "this GPU's Reset follows\n");
        return true;
    }
    // The unlink may precede a failed directory sync: put the exact journal back.
    char restoreErr[256] = {};
    const bool restored = linux_daemon_state_store(GC_DAEMON_STATE_FILE, &journal.record,
                                                   restoreErr, sizeof(restoreErr));
    if (!restored) {
        ctl.journalHoldActive = true;
        ctl.journalHold = journal.record;
    }
    resp->status = SERVICE_STATUS_ERROR;
    gc_snprintf(resp->message, sizeof(resp->message), restored
        ? "Reset refused: cannot release the saved state of a GPU that is not present: %s"
        : "Reset not run: the saved state of a GPU that is not present may already be gone "
          "(%s); this GPU was not reset and stays blocked until storage recovers",
        releaseErr);
    dlog("daemon journal: release failed (%s); journal restored=%d%s%s\n", releaseErr,
         restored ? 1 : 0, restoreErr[0] ? ": " : "", restoreErr);
    return false;
}
