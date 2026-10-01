// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "installer_archive_policy.h"
#include <string.h>

// Execute the actual harness with observation wrappers: a passing parser unit
// test cannot prove that the fuzzer ever calls the parser or reaches success.
static int footerCalls = 0, footerAccepted = 0, archiveAccepted = 0;
static GcPayloadStatus observe_footer(const GcPayloadFooter* footer,
    uint64_t size, uint64_t maximum) {
    ++footerCalls;
    GcPayloadStatus status = gc_payload_validate_footer(footer, size, maximum);
    if (status == GC_PAYLOAD_OK) ++footerAccepted;
    return status;
}
static GcArchiveStatus observe_archive(const void* data, uint64_t size,
    const GcArchiveEntry** entries, uint32_t* count) {
    GcArchiveStatus status = gc_archive_validate(data, size, entries, count);
    if (status == GC_ARCHIVE_OK) ++archiveAccepted;
    return status;
}
#define GC_FUZZ_TARGET 8
#define gc_payload_validate_footer observe_footer
#define gc_archive_validate observe_archive
#define LLVMFuzzerTestOneInput archive_harness_input
#define is_curve_point_visible_in_gui archive_fixture_visible
#define debug_log archive_fixture_log
#define invalidate_tray_profile_cache archive_fixture_invalidate
#include "fuzz_main.cpp"

int run_installer_fuzz_harness_tests() {
    enum { containerBytes = sizeof(GcArchiveHeader) + sizeof(GcArchiveEntry) + 16,
           tailBytes = 5 + sizeof(GcPayloadFooter) + 1 };
    unsigned char input[containerBytes + tailBytes] = {};
    // Steering alone must suffice to reach a valid archive from zeroed bytes.
    unsigned char* tail = input + containerBytes;
    tail[0] = 1; tail[1] = 2; tail[2] = 0; tail[3] = 4; tail[4] = 1;
    GcPayloadFooter footer = {};
    memcpy(footer.magic, GC_PAYLOAD_FOOTER_MAGIC, sizeof(footer.magic));
    footer.method = GC_PAYLOAD_METHOD_STORE;
    footer.archiveOffset = 128;
    footer.compressedSize = footer.uncompressedSize = 320 - sizeof(footer) - 128;
    footer.footerCrc32 = gc_payload_footer_expected_crc(&footer);
    memcpy(tail + 5, &footer, sizeof(footer));
    tail[5 + sizeof(footer)] = 5; // file size = 5 * 64
    archive_harness_input(input, sizeof(input));
    if (!archiveAccepted || !footerAccepted || footerCalls != 1) return 6226;
    // The newly live range-steering branch previously wrote entries[0] beyond
    // a 13-byte malloc when fileCount was zero. ASan must execute this case.
    unsigned char tiny[13 + tailBytes] = {};
    tiny[13 + 3] = 4;
    archive_harness_input(tiny, sizeof(tiny));
    if (footerCalls != 2) return 6227;
    archive_harness_input(input, 0);
    if (footerCalls != 3) return 6228;
    return 0;
}
