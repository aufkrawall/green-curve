// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CRASH_DUMP_NOTICE_POLICY_H
#define GREEN_CURVE_CRASH_DUMP_NOTICE_POLICY_H

// The background service's crash dumps stay in its admin-only machine data
// directory: a SYSTEM process dump can hold another account's data, so they
// are never copied anywhere a standard user can read.  What the user CAN be
// given is the fact that dumps exist and where: a short notice file in their
// own Green Curve folder, written with the user's own rights, listing the
// newest dumps and the elevated command that copies them out.
//
// Pure selection and formatting; the directory scan and the write live in
// main_service_crash_notice.cpp.

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "powershell_literal.h"

#define GC_CRASH_DUMP_NOTICE_FILE_NAME "greencurve_service_crash_dumps.txt"
// Newest entries listed per kind; the totals are always reported.
#define GC_CRASH_DUMP_NOTICE_MAX_LISTED 5u

struct GcCrashDumpNoticeEntry {
    char name[96];
    unsigned long long sizeBytes;
    unsigned long long sortKey;  // last-write time, 100 ns units; larger = newer
    char when[24];               // local "YYYY-MM-DD HH:MM:SS"
};

// Keeps the `keep` newest of `count` entries at the front, newest first.
// Returns how many are kept.  Insertion sort: `count` is bounded by the scan.
static inline size_t gc_crash_dump_notice_keep_newest(GcCrashDumpNoticeEntry* entries,
                                                      size_t count, size_t keep) {
    if (!entries) return 0;
    for (size_t i = 1; i < count; ++i) {
        GcCrashDumpNoticeEntry current = entries[i];
        size_t j = i;
        while (j > 0 && entries[j - 1].sortKey < current.sortKey) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = current;
    }
    return count < keep ? count : keep;
}

static inline size_t gc_crash_dump_notice_append(char* out, size_t outSize, size_t used,
                                                 const char* text) {
    if (!out || outSize == 0 || used >= outSize || !text) return used;
    size_t room = outSize - used - 1;
    size_t n = strlen(text);
    if (n > room) n = room;
    memcpy(out + used, text, n);
    out[used + n] = 0;
    return used + n;
}

static inline size_t gc_crash_dump_notice_append_list(char* out, size_t outSize, size_t used,
                                                      const char* heading, const char* dir,
                                                      const GcCrashDumpNoticeEntry* entries,
                                                      size_t listed, size_t total) {
    char line[320];
    snprintf(line, sizeof(line), "%s (%zu in %s):\r\n", heading, total, dir ? dir : "?");
    used = gc_crash_dump_notice_append(out, outSize, used, line);
    for (size_t i = 0; i < listed; ++i) {
        snprintf(line, sizeof(line), "  %s  %8llu KB  %s\r\n", entries[i].when,
                 (entries[i].sizeBytes + 1023ULL) / 1024ULL, entries[i].name);
        used = gc_crash_dump_notice_append(out, outSize, used, line);
    }
    if (total > listed) {
        snprintf(line, sizeof(line), "  ... and %zu older\r\n", total - listed);
        used = gc_crash_dump_notice_append(out, outSize, used, line);
    }
    return used;
}

// The notice text.  `dumps`/`werDumps` are already reduced to their newest
// `*Listed` entries; `*Total` are the full counts.  Returns false when there
// is nothing to announce (no dumps of either kind), leaving `out` empty.
static inline bool gc_crash_dump_notice_format(
    const char* serviceDir, const char* werDir,
    const GcCrashDumpNoticeEntry* dumps, size_t dumpsListed, size_t dumpsTotal,
    const GcCrashDumpNoticeEntry* werDumps, size_t werListed, size_t werTotal,
    const char* copyDestination, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (dumpsTotal == 0 && werTotal == 0) return false;
    size_t used = 0;
    used = gc_crash_dump_notice_append(out, outSize, used,
        "Green Curve: the background service has crash dumps available.\r\n"
        "\r\n"
        "They help diagnose service crashes (for example during a graphics driver\r\n"
        "update). Only administrators can read them, because a dump of a system\r\n"
        "service can contain data of other accounts on this PC. This file only\r\n"
        "tells you where they are; Green Curve updates it whenever the service\r\n"
        "starts for your session and removes it when no dumps are left.\r\n"
        "\r\n");
    if (dumpsTotal) {
        used = gc_crash_dump_notice_append_list(out, outSize, used,
            "Service crash dumps", serviceDir, dumps, dumpsListed, dumpsTotal);
        used = gc_crash_dump_notice_append(out, outSize, used, "\r\n");
    }
    if (werTotal) {
        used = gc_crash_dump_notice_append_list(out, outSize, used,
            "Windows Error Reporting dumps", werDir, werDumps, werListed, werTotal);
        used = gc_crash_dump_notice_append(out, outSize, used, "\r\n");
    }
    // The elevated command.  Every path is a single-quoted literal and is only
    // ever passed as a literal path, so neither PowerShell expansion nor
    // wildcard matching ([, ], *) can reinterpret a folder name.  The command
    // is all or nothing: a cut-off command could still run, so when it does
    // not fit it is replaced by a plain instruction.
    const size_t commandStart = used;
    bool overflow = false;
    used = gc_crash_dump_notice_append(out, outSize, used,
        "To copy them here, open PowerShell as administrator and run:\r\n"
        "\r\n"
        "  $dst = ");
    used = gc_powershell_append_literal(out, outSize, used,
        copyDestination ? copyDestination : ".", &overflow);
    used = gc_crash_dump_notice_append(out, outSize, used, "; $svc = ");
    used = gc_powershell_append_literal(out, outSize, used,
        serviceDir ? serviceDir : ".", &overflow);
    used = gc_crash_dump_notice_append(out, outSize, used, "; $wer = ");
    used = gc_powershell_append_literal(out, outSize, used,
        werDir ? werDir : ".", &overflow);
    static const char kCommandTail[] =
        "; New-Item -ItemType Directory -Force -Path $dst | Out-Null; "
        "Get-ChildItem -LiteralPath $svc, $wer -Filter *.dmp -File -ErrorAction SilentlyContinue "
        "| Copy-Item -Destination $dst; "
        "Copy-Item -LiteralPath (Join-Path $svc 'greencurve_crash.txt') -Destination $dst "
        "-ErrorAction SilentlyContinue\r\n";
    if (overflow || outSize - used <= sizeof(kCommandTail) - 1) {
        out[commandStart] = 0;
        gc_crash_dump_notice_append(out, outSize, commandStart,
            "To copy them, open the folders above as an administrator.\r\n");
        return true;
    }
    gc_crash_dump_notice_append(out, outSize, used, kCommandTail);
    return true;
}

#endif  // GREEN_CURVE_CRASH_DUMP_NOTICE_POLICY_H
