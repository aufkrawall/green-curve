// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Whose GPU the daemon's saved journal belongs to, and which writes it admits
// (linux_journal_owner_policy.h). Codes 14900-14923, 14926-14927, 14929, 14940, 14947.
#if !defined(_WIN32)
#include "gpu_core.h"
#include "linux_journal_owner_policy.h"
#include "linux_graphics_recovery_policy.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
GpuAdapterInfo journal_gpu(unsigned int bus, unsigned int device = 0x1E07) {
    GpuAdapterInfo gpu = {};
    gpu.valid = gpu.pciInfoValid = true;
    gpu.deviceId = device;
    gpu.subSystemId = 0x12FF10DE;
    gpu.pciBus = bus;
    return gpu;
}
LinuxGpuUuid journal_uuid(char c) {
    char text[48] = {};
    snprintf(text, sizeof(text), "GPU-%c%c%c%c%c%c%c%c-1111-2222-3333-444444444444",
             c, c, c, c, c, c, c, c);
    return linux_gpu_uuid_parse(text);
}
}  // namespace

int run_linux_journal_owner_tests() {
    // Two different cards, the first served; discovery listed all NVML reported.
    GpuAdapterInfo live[2] = {journal_gpu(0x21), journal_gpu(0x4B, 0x2204)};
    LinuxGpuUuid uuids[2] = {journal_uuid('a'), journal_uuid('b')};
    LinuxJournalEvidence e = {live, uuids, 2, 2, 0};
    const LinuxGpuUuid none = {};
    int owner = -9;

    // The served card's own journal, by location or by UUID after a move.
    if (linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, none, e, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED || owner != 0) return 14900;
    GpuAdapterInfo moved = live[0];
    moved.pciBus = 0x09;
    if (linux_journal_owner(moved, GPU_LOCATION_ROLE_REQUIRED, uuids[0], e, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14901;

    // Another present card's journal is that card's: every write refuses.
    if (linux_journal_owner(live[1], GPU_LOCATION_ROLE_UNRECORDED, uuids[1], e, &owner) !=
            LINUX_JOURNAL_OWNER_OTHER || owner != 1) return 14902;
    for (LinuxJournalWrite w : {LINUX_JOURNAL_WRITE_APPLY, LINUX_JOURNAL_WRITE_RESET,
                                LINUX_JOURNAL_WRITE_AUTOMATIC})
        if (linux_journal_write_decision(LINUX_JOURNAL_OWNER_OTHER, w, true) !=
                LINUX_JOURNAL_REFUSE) return 14903;

    // A card no longer present: only an explicit Reset of the served card
    // releases its journal, and only settled intent -- never unfinished
    // recovery, whatever the number of cards.
    const GpuAdapterInfo removed = journal_gpu(0x65, 0x2684);
    if (linux_journal_owner(removed, GPU_LOCATION_ROLE_UNRECORDED, journal_uuid('c'), e, &owner) !=
            LINUX_JOURNAL_OWNER_GONE || owner != -1) return 14904;
    if (linux_journal_write_decision(LINUX_JOURNAL_OWNER_GONE, LINUX_JOURNAL_WRITE_RESET, true) !=
            LINUX_JOURNAL_RELEASE ||
        linux_journal_write_decision(LINUX_JOURNAL_OWNER_GONE, LINUX_JOURNAL_WRITE_RESET, false) !=
            LINUX_JOURNAL_REFUSE ||
        linux_journal_write_decision(LINUX_JOURNAL_OWNER_GONE, LINUX_JOURNAL_WRITE_APPLY, true) !=
            LINUX_JOURNAL_REFUSE ||
        linux_journal_write_decision(LINUX_JOURNAL_OWNER_GONE, LINUX_JOURNAL_WRITE_AUTOMATIC, true) !=
            LINUX_JOURNAL_REFUSE) return 14905;

    // "Not found" is not "gone" when discovery skipped a reported device or
    // listed one without PCI identity: that device could be the owner.
    LinuxJournalEvidence partial = e;
    partial.reportedCount = 3;
    if (linux_journal_owner(removed, GPU_LOCATION_ROLE_UNRECORDED, none, partial, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14906;
    GpuAdapterInfo blind[2] = {live[0], live[1]};
    blind[1].pciInfoValid = false;
    LinuxJournalEvidence noPci = {blind, uuids, 2, 2, 0};
    if (linux_journal_owner(removed, GPU_LOCATION_ROLE_UNRECORDED, none, noPci, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14907;
    for (LinuxJournalWrite w : {LINUX_JOURNAL_WRITE_APPLY, LINUX_JOURNAL_WRITE_RESET,
                                LINUX_JOURNAL_WRITE_AUTOMATIC})
        if (linux_journal_write_decision(LINUX_JOURNAL_OWNER_UNPROVEN, w, true) !=
                LINUX_JOURNAL_REFUSE ||
            linux_journal_write_decision(LINUX_JOURNAL_OWNER_UNREADABLE, w, true) !=
                LINUX_JOURNAL_REFUSE) return 14908;

    // An unresolved journal another present card could own (same model, or
    // its UUID) is unproven, not gone.
    GpuAdapterInfo twins[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    LinuxJournalEvidence twinEvidence = {twins, nullptr, 2, 2, 0};
    if (linux_journal_owner(journal_gpu(0x65), GPU_LOCATION_ROLE_REQUIRED, none, twinEvidence, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14909;
    GpuAdapterInfo otherModel = journal_gpu(0x77, 0x2684);
    if (linux_journal_owner(otherModel, GPU_LOCATION_ROLE_UNRECORDED, uuids[1], e, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14910;

    // A lone card follows its model across a renumbering as before; at a
    // location its required role no longer accepts, with no other card of its
    // model, only the served card can be it, so its Reset may release it.
    GpuAdapterInfo single[1] = {journal_gpu(0x21)};
    LinuxJournalEvidence alone = {single, nullptr, 1, 1, 0};
    if (linux_journal_owner(journal_gpu(0x09), GPU_LOCATION_ROLE_UNRECORDED, none, alone, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14911;
    if (linux_journal_owner(journal_gpu(0x09), GPU_LOCATION_ROLE_REQUIRED, none, alone, &owner) !=
            LINUX_JOURNAL_OWNER_GONE) return 14915;
    GpuAdapterInfo pair[2] = {journal_gpu(0x21), journal_gpu(0x4B, 0x2204)};
    LinuxJournalEvidence required = {pair, nullptr, 2, 2, 0};
    if (linux_journal_owner(journal_gpu(0x09), GPU_LOCATION_ROLE_REQUIRED, none, required, &owner) !=
            LINUX_JOURNAL_OWNER_GONE) return 14912;
    // Nothing served: nothing can be proven gone.
    LinuxJournalEvidence unserved = e;
    unserved.servedIndex = -1;
    if (linux_journal_owner(removed, GPU_LOCATION_ROLE_UNRECORDED, none, unserved, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14913;
    if (linux_journal_write_decision(LINUX_JOURNAL_OWNER_NONE, LINUX_JOURNAL_WRITE_AUTOMATIC, false) !=
            LINUX_JOURNAL_ALLOW ||
        linux_journal_write_decision(LINUX_JOURNAL_OWNER_SERVED, LINUX_JOURNAL_WRITE_APPLY, false) !=
            LINUX_JOURNAL_ALLOW) return 14914;
    // Discovery skipped a reported device (NVML listed one of two identical
    // cards): a journal whose location moved may belong to the skipped card,
    // so following its model to the served card is not proof. Nor is its
    // UUID: the skipped card was not read and may report the same one. Only
    // a journal without a UUID at its exact location is proven.
    GpuAdapterInfo seen[1] = {journal_gpu(0x4B)};
    LinuxGpuUuid seenUuid[1] = {journal_uuid('b')};
    LinuxJournalEvidence skipped = {seen, seenUuid, 1, 2, 0};
    if (linux_journal_owner(journal_gpu(0x21), GPU_LOCATION_ROLE_UNRECORDED, none, skipped, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14916;
    if (linux_journal_owner(journal_gpu(0x21), GPU_LOCATION_ROLE_UNRECORDED, journal_uuid('b'), skipped,
            &owner) != LINUX_JOURNAL_OWNER_UNPROVEN ||
        linux_journal_owner(journal_gpu(0x4B), GPU_LOCATION_ROLE_UNRECORDED, none, skipped, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14917;
    // 14947: a journal with a UUID is proven only by that UUID while discovery
    // skipped a device, even at its exact location: the card there may be
    // another one now, its own last-known location the unopened device.
    LinuxGpuUuid unreadable[1] = {};
    LinuxJournalEvidence skippedUnread = {seen, unreadable, 1, 2, 0};
    if (linux_journal_owner(journal_gpu(0x4B), GPU_LOCATION_ROLE_UNRECORDED, journal_uuid('a'), skippedUnread,
            &owner) != LINUX_JOURNAL_OWNER_UNPROVEN ||
        linux_journal_owner(journal_gpu(0x4B), GPU_LOCATION_ROLE_UNRECORDED, none, skippedUnread, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14947;
    // Two identical cards, one replaced since the journal was written: both
    // report their own unique UUIDs, neither is the journal's, so it is gone
    // rather than unproven -- otherwise neither card could ever be Reset.
    GpuAdapterInfo pairTwins[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    LinuxGpuUuid pairUuids[2] = {journal_uuid('a'), journal_uuid('b')};
    LinuxJournalEvidence replaced = {pairTwins, pairUuids, 2, 2, 0};
    if (linux_journal_owner(journal_gpu(0x4B), GPU_LOCATION_ROLE_REQUIRED, journal_uuid('c'), replaced,
            &owner) != LINUX_JOURNAL_OWNER_GONE) return 14918;
    LinuxGpuUuid sameUuids[2] = {journal_uuid('a'), journal_uuid('a')};
    LinuxJournalEvidence duplicated = {pairTwins, sameUuids, 2, 2, 0};
    if (linux_journal_owner(journal_gpu(0x65), GPU_LOCATION_ROLE_REQUIRED, journal_uuid('c'), duplicated,
            &owner) != LINUX_JOURNAL_OWNER_UNPROVEN) return 14918;
    // A journal with no recorded location follows its model, which proves
    // nothing while discovery skipped a device (14920).
    GpuAdapterInfo noLocation = journal_gpu(0);
    noLocation.pciBus = 0;
    if (linux_journal_owner(noLocation, GPU_LOCATION_ROLE_UNRECORDED, none, skipped, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14920;
    // Rediscovery carries a UUID an earlier discovery read for the same exact
    // location when this read failed, never one this read found elsewhere
    // (14921); with it, a known mismatch stays a mismatch and a card proven by
    // UUID at a new location is still found there (14922).
    GpuAdapterInfo before[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    LinuxGpuUuid beforeUuids[2] = {journal_uuid('b'), journal_uuid('a')};
    GpuAdapterInfo after[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    LinuxGpuUuid afterUuids[2] = {};
    afterUuids[1] = journal_uuid('a');
    if (linux_gpu_uuid_carry_forward(before, beforeUuids, 2, after, afterUuids, 2) != 1 ||
        strcmp(afterUuids[0].text, beforeUuids[0].text)) return 14921;
    LinuxGpuUuid stale[2] = {};
    stale[1] = journal_uuid('b');
    if (linux_gpu_uuid_carry_forward(before, beforeUuids, 2, after, stale, 2) != 0 ||
        linux_gpu_uuid_valid(&stale[0])) return 14921;
    // A UUID the earlier discovery saw on two adapters is no proof for
    // either; and copies made in this pass never vouch for one another.
    LinuxGpuUuid twinBefore[2] = {journal_uuid('d'), journal_uuid('d')};
    LinuxGpuUuid twinAfter[2] = {};
    if (linux_gpu_uuid_carry_forward(before, twinBefore, 2, after, twinAfter, 2) != 0 ||
        linux_gpu_uuid_valid(&twinAfter[0]) || linux_gpu_uuid_valid(&twinAfter[1])) return 14923;
    // Earlier evidence that a rediscovered choice must answer with fresh reads
    // (14926): one readable claimant does not resolve a known duplicate; a
    // second claimant reading something else now does; a known mismatch at the
    // chosen location needs the stored UUID read there.
    LinuxGpuUuid dupBefore[2] = {journal_uuid('e'), journal_uuid('e')};
    LinuxGpuUuid oneRead[2] = {journal_uuid('e'), LinuxGpuUuid{}};
    LinuxGpuUuid bothRead[2] = {journal_uuid('e'), journal_uuid('f')};
    const LinuxGpuUuid storedE = journal_uuid('e');
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, after, oneRead, 2, 0, true) ||
        linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, after, bothRead, 2, 0, true))
        return 14926;
    LinuxGpuUuid otherBefore[2] = {journal_uuid('f'), LinuxGpuUuid{}};
    LinuxGpuUuid noneRead[2] = {};
    LinuxGpuUuid provenRead[2] = {journal_uuid('e'), LinuxGpuUuid{}};
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, otherBefore, 2, after, noneRead, 2, 0, true) ||
        linux_gpu_uuid_known_evidence_unresolved(&storedE, before, otherBefore, 2, after, provenRead, 2, 0, true) ||
        linux_gpu_uuid_known_evidence_unresolved(nullptr, before, otherBefore, 2, after, noneRead, 2, 0, true))
        return 14926;
    // Missing optional metadata never detaches earlier evidence from its
    // location, and a claimant not listed is absent only when discovery was
    // complete (14927).
    GpuAdapterInfo noSubsystem[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    noSubsystem[0].subSystemId = 0;
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, otherBefore, 2, noSubsystem, noneRead,
            2, 0, true)) return 14927;
    GpuAdapterInfo onlyFirst[1] = {journal_gpu(0x21)};
    LinuxGpuUuid firstRead[1] = {journal_uuid('e')};
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, onlyFirst, firstRead,
            1, 0, false) ||
        linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, onlyFirst, firstRead,
            1, 0, true)) return 14927;
    // An earlier claimant missing from its old location while a card of its
    // model sits elsewhere without a fresh, different UUID may be that
    // claimant, moved (14929).
    GpuAdapterInfo movedTwin[2] = {journal_gpu(0x21), journal_gpu(0x60)};
    LinuxGpuUuid movedRead[2] = {journal_uuid('e'), LinuxGpuUuid{}};
    LinuxGpuUuid movedRuledOut[2] = {journal_uuid('e'), journal_uuid('f')};
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, movedTwin, movedRead,
            2, 0, true) ||
        linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, movedTwin, movedRuledOut,
            2, 0, true)) return 14929;
    // A third card now at the claimant's old location reading another UUID
    // rules out only itself; the claimant may sit elsewhere, unread.
    GpuAdapterInfo thirdCard[3] = {journal_gpu(0x21), journal_gpu(0x4B), journal_gpu(0x60)};
    LinuxGpuUuid thirdRead[3] = {journal_uuid('e'), journal_uuid('f'), LinuxGpuUuid{}};
    if (!linux_gpu_uuid_known_evidence_unresolved(&storedE, before, dupBefore, 2, thirdCard, thirdRead,
            3, 0, true)) return 14929;
    LinuxGpuUuidVerdict verdict = LINUX_GPU_UUID_FALLBACK;
    const LinuxGpuUuid absentCard = journal_uuid('c');
    const auto contradicted = linux_gpu_restore_selection(&after[0], GPU_LOCATION_ROLE_REQUIRED,
        after, 2, &absentCard, afterUuids, &verdict);
    const auto relocated = linux_gpu_restore_selection(&after[0], GPU_LOCATION_ROLE_REQUIRED,
        after, 2, &afterUuids[1], afterUuids, &verdict);
    if (contradicted.writeResolved || !relocated.writeResolved || relocated.index != 1) return 14922;
    // 14940: with several GPUs a journal naming a UUID is proven only by a
    // current read of that UUID, never by its location; one without a UUID,
    // and a single GPU, keep the location rule.
    LinuxGpuUuid emptyReads[2] = {};
    LinuxJournalEvidence unread = {live, emptyReads, 2, 2, 0};
    if (linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, uuids[0], unread, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN ||
        linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, none, unread, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED ||
        linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, uuids[0], e, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14940;
    // ...and only while every listed card was read: a UUID that looks unique
    // among partial reads may be a duplicate of an unread card.
    LinuxGpuUuid partialReads[2] = {uuids[0], LinuxGpuUuid{}};
    LinuxJournalEvidence partialRead = {live, partialReads, 2, 2, 0};
    if (linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, uuids[0], partialRead, &owner) !=
            LINUX_JOURNAL_OWNER_UNPROVEN) return 14940;
    // A journal's card is not released as gone on partial reads either: an
    // unread twin may report the same UUID as the one that was read.
    GpuAdapterInfo partialTwins[2] = {journal_gpu(0x21), journal_gpu(0x4B)};
    LinuxGpuUuid twinPartial[2] = {LinuxGpuUuid{}, journal_uuid('a')};
    LinuxJournalEvidence twinsPartial = {partialTwins, twinPartial, 2, 2, 0};
    if (linux_journal_owner(journal_gpu(0x60), GPU_LOCATION_ROLE_REQUIRED, journal_uuid('c'), twinsPartial,
            &owner) != LINUX_JOURNAL_OWNER_UNPROVEN) return 14940;
    LinuxJournalEvidence singleUnread = {live, emptyReads, 1, 1, 0};
    if (linux_journal_owner(live[0], GPU_LOCATION_ROLE_UNRECORDED, uuids[0], singleUnread, &owner) !=
            LINUX_JOURNAL_OWNER_SERVED) return 14940;
    // Several GPUs' journals in one file are a boot that needs recovery.
    const LinuxDaemonStateRecord noRecord = {};
    if (linux_daemon_boot_action(SERVICE_STARTUP_POLICY_RESTORE_LAST, LINUX_DAEMON_STATE_MULTIPLE,
            noRecord) != LINUX_DAEMON_BOOT_RECOVERY ||
        linux_daemon_boot_action(SERVICE_STARTUP_POLICY_NONE, LINUX_DAEMON_STATE_MULTIPLE,
            noRecord) != LINUX_DAEMON_BOOT_RECOVERY) return 14919;
    return 0;
}
#else
int run_linux_journal_owner_tests() { return 0; }
#endif
