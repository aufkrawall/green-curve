// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Post-0.27.0 review follow-ups (codes 6250-6269): the freshness refusal
// classification that tells "not renewed yet" from "wrong clock", and the
// Linux atomic writer's path rules.  Both are pure, so they run on every host.
#include "update_freshness_policy.h"
#include "linux_atomic_write_policy.h"
#include "app_shared.h"
#include <string.h>
#include <string>

static int freshness_status_tests() {
    const long long issued = 2000000000LL;
    const long long expires = issued + 100;
    const char envelope[] =
        "freshness=1\nissued=2000000000\nexpires=2000000100\nformat=1\n";
    const size_t size = sizeof(envelope) - 1;
    GcUpdateFreshness fresh = {};

    if (gc_update_fresh_parse_status(envelope, size, issued, &fresh) != GC_UPDATE_FRESH_OK ||
        fresh.manifestOffset == 0) return 6250;
    // At and after expiry: the publisher's renewal is overdue, not this clock.
    if (gc_update_fresh_parse_status(envelope, size, expires, &fresh) !=
        GC_UPDATE_FRESH_EXPIRED || fresh.manifestOffset != 0) return 6251;
    // Beyond the skew allowance: this PC's clock is behind.
    if (gc_update_fresh_parse_status(envelope, size,
            issued - GC_UPDATE_FRESH_CLOCK_SKEW - 1, &fresh) != GC_UPDATE_FRESH_FUTURE)
        return 6252;
    if (gc_update_fresh_parse_status(envelope, size, 0, &fresh) != GC_UPDATE_FRESH_NO_CLOCK)
        return 6253;
    const char legacy[] = "format=1\nversion=1.2.3\n";
    if (gc_update_fresh_parse_status(legacy, sizeof(legacy) - 1, issued, &fresh) !=
        GC_UPDATE_FRESH_MALFORMED || fresh.issued != 0) return 6254;
    GcUpdateFreshness tooLong = {issued, issued + GC_UPDATE_FRESH_MAX_LIFETIME + 1, 0};
    if (gc_update_fresh_time_status(&tooLong, issued) != GC_UPDATE_FRESH_BAD_LIFETIME)
        return 6255;
    // The bool API is exactly "status is OK", so every existing gate and
    // caller keeps its meaning.
    if (gc_update_fresh_time_valid(&fresh, issued) !=
        (gc_update_fresh_time_status(&fresh, issued) == GC_UPDATE_FRESH_OK)) return 6256;
    // Expired and wrong-clock must not read the same: one says "wait", the
    // other says "fix the clock", and that remedy is the reason they differ.
    const char* expiredText = gc_update_fresh_status_text(GC_UPDATE_FRESH_EXPIRED);
    const char* futureText = gc_update_fresh_status_text(GC_UPDATE_FRESH_FUTURE);
    if (!expiredText || !futureText || strcmp(expiredText, futureText) == 0 ||
        strstr(expiredText, "clock") || !strstr(futureText, "date and time")) return 6257;
    for (int status = GC_UPDATE_FRESH_OK; status <= GC_UPDATE_FRESH_EXPIRED; ++status) {
        const char* text = gc_update_fresh_status_text((GcUpdateFreshStatus)status);
        if (!text || !text[0] || strlen(text) >= 128) return 6258;
    }
    return 0;
}

static int atomic_write_path_tests() {
    std::string dir, name;
    // A bare file name is the current directory; it used to be refused.
    if (!linux_atomic_write_split("report.md", &dir, &name) ||
        dir != "." || name != "report.md") return 6260;
    if (!linux_atomic_write_split("/greencurve.ini", &dir, &name) ||
        dir != "/" || name != "greencurve.ini") return 6261;
    if (!linux_atomic_write_split("/home/u/.config/greencurve/greencurve.ini", &dir, &name) ||
        dir != "/home/u/.config/greencurve" || name != "greencurve.ini") return 6262;
    if (!linux_atomic_write_split("rel/dir/x.ini", &dir, &name) ||
        dir != "rel/dir" || name != "x.ini") return 6263;
    for (const char* bad : {"", "/", "dir/", "dir/.", "dir/..", ".", ".."}) {
        if (linux_atomic_write_split(bad, &dir, &name)) return 6264;
    }
    // The temp name stays inside NAME_MAX for the longest valid leaf.
    char temporary[LINUX_ATOMIC_WRITE_TEMP_PREFIX_MAX + 32] = {};
    std::string longest(255, 'a');
    if (!linux_atomic_write_temp_name(longest, 0xFFFFFFFFFFFFFFFFull, temporary,
                                      sizeof(temporary)) ||
        strlen(temporary) > 255 || temporary[0] != '.') return 6265;
    if (!linux_atomic_write_temp_name("x.ini", 0x1ull, temporary, sizeof(temporary)) ||
        strcmp(temporary, ".x.ini.tmp.0000000000000001") != 0) return 6266;
    if (linux_atomic_write_temp_name("x.ini", 1, temporary, 8)) return 6267;
    return 0;
}

static int string_copy_shim_tests() {
    char buf[16] = {};
    HRESULT hr = StringCchCopyA(buf, sizeof(buf), "hello");
    if (!SUCCEEDED(hr) || hr != S_OK || strcmp(buf, "hello") != 0) return 6270;

    hr = StringCchCopyA(buf, sizeof(buf), "");
    if (!SUCCEEDED(hr) || hr != S_OK || buf[0] != '\0') return 6271;

    const char exact15[] = "123456789012345";
    hr = StringCchCopyA(buf, sizeof(buf), exact15);
    if (!SUCCEEDED(hr) || hr != S_OK || strcmp(buf, exact15) != 0) return 6272;

    const char overlong[] = "12345678901234567890";
    hr = StringCchCopyA(buf, sizeof(buf), overlong);
    if (SUCCEEDED(hr) || !FAILED(hr) || hr != STRSAFE_E_INSUFFICIENT_BUFFER) return 6273;
    if (buf[sizeof(buf) - 1] != '\0' || strncmp(buf, overlong, sizeof(buf) - 1) != 0) return 6274;

    if (!FAILED(StringCchCopyA(buf, 0, "test"))) return 6275;

    if (!SUCCEEDED(S_OK) || FAILED(S_OK)) return 6276;
    if (!SUCCEEDED(S_FALSE) || FAILED(S_FALSE)) return 6277;

    return 0;
}

int run_review_followup_tests() {
    if (int failure = freshness_status_tests()) return failure;
    if (int failure = atomic_write_path_tests()) return failure;
    if (int failure = string_copy_shim_tests()) return failure;
    return 0;
}
