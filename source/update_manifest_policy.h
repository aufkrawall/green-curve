// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The signed release manifest: format, parsing, and the binding rules that
// decide whether a downloaded setup executable may be run at all.
//
// ## Why a manifest exists
//
// The updater's trust root is an ECDSA P-256 public key compiled into the
// binary, whose private half never enters GitHub Actions.  That key signs
// *this file*, and this file is what names the artifact.  The chain is:
//
//     embedded public key -> manifest signature -> sha256 + exact byte size
//                         -> the setup executable on disk
//
// Everything else in the pipeline is defense in depth around that chain, and
// two of the obvious candidates are much weaker than they look:
//
//   - The published `.sha256` files sit in the same release, on the same host,
//     under the same account as the `.exe`.  Whoever can swap one can swap the
//     other, so they detect corruption and truncation and contribute nothing
//     to authenticity.
//   - The GitHub build-provenance attestation certifies "CI built this from
//     repo X at commit Y".  A compromised account pushes a commit, runs the
//     release workflow, and gets a genuinely valid attestation for hostile
//     code.  It is excellent forensics and not a gate.
//
// ## The signature is checked over raw bytes, before this parser runs
//
// The signature covers the manifest file's exact bytes, so there is no
// canonicalization step and no way for a re-serialization difference to move
// the signed boundary.  The caller MUST verify the signature first and only
// then call `gc_update_manifest_parse()`, which means this parser normally
// sees only authenticated input.  It is still written to survive arbitrary
// bytes -- and is fuzzed as an untrusted boundary -- because "the verifier is
// correct" is exactly the assumption that should not be load-bearing twice.
//
// ## Format
//
//     # comments and blank lines are allowed
//     format=1
//     version=0.23.0
//     min_from=0.21
//     x64_file=greencurve-0.23.0-windows-x64-setup.exe
//     x64_size=4712345
//     x64_sha256=<64 lowercase hex>
//     arm64_file=greencurve-0.23.0-windows-arm64-setup.exe
//     arm64_size=4698123
//     arm64_sha256=<64 lowercase hex>
//
// Strictness is deliberate throughout, and mirrors the installer's decision to
// reject unknown command-line switches rather than ignore them:
//
//   - An **unknown key is rejected**, not skipped.  Forward compatibility is
//     `format=`: a future layout bumps it and every older client refuses the
//     whole document instead of half-understanding it.
//   - A **duplicate key is rejected**.  "Last one wins" is a smuggling
//     primitive -- two `x64_sha256` lines should never be a question of which
//     parser you are talking to.
//   - **No whitespace tolerance** around `=`.  The manifest is machine-written
//     by `tools/update_signing.py`; a human never types it, so accepting
//     sloppy input only widens the grammar an attacker can aim at.
//
// ## The mix-and-match binding
//
// Per-arch `file`/`size`/`sha256` are validated *together*, and the filename
// must equal the name the release workflow actually produces for this exact
// version and architecture.  Without that, a signed manifest for 0.23 could
// name the 0.23 arm64 asset in the x64 slot, or an attacker could rename an
// older signed asset into a newer release and satisfy a hash check that only
// looked at the bytes.

#ifndef GREEN_CURVE_UPDATE_MANIFEST_POLICY_H
#define GREEN_CURVE_UPDATE_MANIFEST_POLICY_H

#include <stddef.h>

#include "update_version_policy.h"

// A manifest is a handful of short lines; anything larger is not one.
#define GC_UPDATE_MANIFEST_MAX_BYTES 4096
#define GC_UPDATE_MANIFEST_MAX_LINE 512
#define GC_UPDATE_ASSET_NAME_MAX_CHARS 128
#define GC_UPDATE_SHA256_HEX_CHARS 64
// The setup executables are single-digit megabytes.  A ceiling this far above
// that is not a size guess -- it bounds how much an attacker who controls the
// response can make the service download before the length check fails.
#define GC_UPDATE_ASSET_MAX_BYTES 268435456ULL
#define GC_UPDATE_MANIFEST_FORMAT 1

enum GcUpdateArch {
    GC_UPDATE_ARCH_UNKNOWN = 0,
    GC_UPDATE_ARCH_X64 = 1,
    GC_UPDATE_ARCH_ARM64 = 2,
};

static inline const char* gc_update_arch_name(GcUpdateArch arch) {
    if (arch == GC_UPDATE_ARCH_X64) return "x64";
    if (arch == GC_UPDATE_ARCH_ARM64) return "arm64";
    return "";
}

struct GcUpdateAsset {
    char file[GC_UPDATE_ASSET_NAME_MAX_CHARS];
    unsigned long long size;
    char sha256[GC_UPDATE_SHA256_HEX_CHARS + 1];
    bool present;
};

struct GcUpdateManifest {
    GcUpdateVersion version;
    GcUpdateVersion minimumFrom;
    bool hasMinimumFrom;
    GcUpdateAsset x64;
    GcUpdateAsset arm64;
    bool valid;
    char error[160];
};

// ---------------------------------------------------------------------------
// Small character helpers.  Spelled out rather than taken from <ctype.h>
// because those are locale-sensitive and this grammar is not.
// ---------------------------------------------------------------------------

static inline bool gc_update_is_digit(char c) { return c >= '0' && c <= '9'; }

static inline bool gc_update_is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static inline void gc_update_manifest_fail(GcUpdateManifest* out,
                                           const char* message,
                                           const char* detail) {
    if (!out) return;
    out->valid = false;
    size_t i = 0;
    while (message && message[i] && i + 1 < sizeof(out->error)) {
        out->error[i] = message[i];
        i++;
    }
    size_t d = 0;
    while (detail && detail[d] && i + 1 < sizeof(out->error)) {
        out->error[i++] = detail[d++];
    }
    out->error[i] = 0;
}

// A release asset name is a bare filename.  This repeats the payload-name
// paranoia from installer_archive_policy.h on purpose: the name reaches a
// path join, and a name is REFUSED rather than sanitized, because sanitizing
// turns a hostile name into a plausible one.
static inline bool gc_update_asset_name_is_acceptable(const char* name) {
    if (!name || !name[0]) return false;
    size_t length = 0;
    while (name[length]) {
        unsigned char c = (unsigned char)name[length];
        if (c < 0x20 || c == 0x7F) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return false;
        length++;
        if (length >= GC_UPDATE_ASSET_NAME_MAX_CHARS) return false;
    }
    // Trailing dots and spaces are silently stripped by Win32 path handling,
    // so "evil.exe " and "evil.exe" name the same file through two different
    // strings -- one of which would sail past a name comparison.
    if (name[length - 1] == '.' || name[length - 1] == ' ') return false;
    if (name[0] == '.') return false;
    return true;
}

// Build the asset name the release workflow produces for this version+arch.
// The version is used as raw TEXT: `release.yml` interpolates the VERSION file
// verbatim, so a manifest saying `0.23` names `...-0.23-windows-...` while one
// saying `0.23.0` names something else, even though the two compare equal.
static inline bool gc_update_expected_asset_name(const char* versionText,
                                                 GcUpdateArch arch,
                                                 char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (!versionText || !versionText[0]) return false;
    const char* archName = gc_update_arch_name(arch);
    if (!archName[0]) return false;

    const char* prefix = "greencurve-";
    const char* middle = "-windows-";
    const char* suffix = "-setup.exe";
    size_t at = 0;
    const char* parts[5] = { prefix, versionText, middle, archName, suffix };
    for (int p = 0; p < 5; ++p) {
        for (size_t i = 0; parts[p][i]; ++i) {
            if (at + 1 >= outSize) { out[0] = 0; return false; }
            out[at++] = parts[p][i];
        }
    }
    out[at] = 0;
    return true;
}

static inline bool gc_update_text_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

// ---------------------------------------------------------------------------
// Field parsers
// ---------------------------------------------------------------------------

// Exactly 64 lowercase hex characters.  Uppercase is refused rather than
// folded so that the manifest has one spelling of a digest and comparisons
// stay a plain byte match with no case table in the path.
static inline bool gc_update_parse_sha256(const char* value, char* out) {
    if (!value || !out) return false;
    out[0] = 0;
    size_t i = 0;
    for (; i < GC_UPDATE_SHA256_HEX_CHARS; ++i) {
        if (!gc_update_is_lower_hex(value[i])) return false;
        out[i] = value[i];
    }
    if (value[i] != 0) return false;   // too long
    out[GC_UPDATE_SHA256_HEX_CHARS] = 0;
    return true;
}

static inline bool gc_update_parse_size(const char* value,
                                        unsigned long long* out) {
    if (!value || !out) return false;
    *out = 0;
    if (!gc_update_is_digit(value[0])) return false;
    if (value[0] == '0' && value[1] != 0) return false;   // no leading zeros
    unsigned long long total = 0;
    for (size_t i = 0; value[i]; ++i) {
        if (!gc_update_is_digit(value[i])) return false;
        if (total > GC_UPDATE_ASSET_MAX_BYTES) return false;
        total = total * 10ULL + (unsigned long long)(value[i] - '0');
    }
    if (total == 0 || total > GC_UPDATE_ASSET_MAX_BYTES) return false;
    *out = total;
    return true;
}

static inline bool gc_update_copy_value(char* dst, size_t dstSize,
                                        const char* src) {
    if (!dst || dstSize == 0) return false;
    dst[0] = 0;
    if (!src) return false;
    size_t i = 0;
    while (src[i]) {
        if (i + 1 >= dstSize) return false;
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i > 0;
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------

// `text`/`length` are the raw manifest bytes, NOT assumed NUL-terminated --
// they arrive from a network read.  Returns with `out->valid` set only when
// every required field parsed AND every cross-field binding held.
static inline void gc_update_manifest_parse(const char* text, size_t length,
                                            GcUpdateManifest* out) {
    if (!out) return;
    GcUpdateManifest blank = {};
    *out = blank;
    if (!text) { gc_update_manifest_fail(out, "No manifest data", nullptr); return; }
    if (length == 0 || length > GC_UPDATE_MANIFEST_MAX_BYTES) {
        gc_update_manifest_fail(out, "Manifest size is out of range", nullptr);
        return;
    }

    bool sawFormat = false, sawVersion = false, sawMinFrom = false;
    bool sawFile[3] = {}, sawSize[3] = {}, sawHash[3] = {};

    size_t cursor = 0;
    while (cursor < length) {
        // Split off one line.  A bare CR is not a terminator; only LF is, and
        // a CR immediately before it is stripped so a CRLF file parses.
        size_t start = cursor;
        while (cursor < length && text[cursor] != '\n') cursor++;
        size_t end = cursor;
        if (cursor < length) cursor++;            // step over the LF
        if (end > start && text[end - 1] == '\r') end--;

        size_t lineLength = end - start;
        if (lineLength == 0) continue;            // blank line
        if (text[start] == '#') continue;         // comment
        if (lineLength >= GC_UPDATE_MANIFEST_MAX_LINE) {
            gc_update_manifest_fail(out, "Manifest line is too long", nullptr);
            return;
        }

        // Copy into a NUL-terminated scratch line so the field parsers can be
        // ordinary C-string code.  An embedded NUL is a rejection: it would
        // otherwise truncate a value that the signer never wrote.
        char line[GC_UPDATE_MANIFEST_MAX_LINE];
        for (size_t i = 0; i < lineLength; ++i) {
            if (text[start + i] == 0) {
                gc_update_manifest_fail(out, "Manifest contains a NUL byte", nullptr);
                return;
            }
            line[i] = text[start + i];
        }
        line[lineLength] = 0;

        char* equals = nullptr;
        for (size_t i = 0; i < lineLength; ++i) {
            if (line[i] == '=') { equals = line + i; break; }
        }
        if (!equals || equals == line) {
            gc_update_manifest_fail(out, "Malformed manifest line: ", line);
            return;
        }
        *equals = 0;
        const char* key = line;
        const char* value = equals + 1;
        if (!value[0]) {
            gc_update_manifest_fail(out, "Empty manifest value for key: ", key);
            return;
        }

        if (gc_update_text_equals(key, "format")) {
            if (sawFormat) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawFormat = true;
            unsigned long long formatValue = 0;
            if (!gc_update_parse_size(value, &formatValue) ||
                formatValue != (unsigned long long)GC_UPDATE_MANIFEST_FORMAT) {
                gc_update_manifest_fail(out, "Unsupported manifest format: ", value);
                return;
            }
            continue;
        }
        if (gc_update_text_equals(key, "version")) {
            if (sawVersion) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawVersion = true;
            gc_update_version_parse(value, &out->version);
            if (!out->version.valid) {
                gc_update_manifest_fail(out, "Unparseable version: ", value);
                return;
            }
            continue;
        }
        if (gc_update_text_equals(key, "min_from")) {
            if (sawMinFrom) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawMinFrom = true;
            gc_update_version_parse(value, &out->minimumFrom);
            if (!out->minimumFrom.valid) {
                gc_update_manifest_fail(out, "Unparseable min_from: ", value);
                return;
            }
            out->hasMinimumFrom = true;
            continue;
        }

        // Per-architecture triples.
        GcUpdateArch arch = GC_UPDATE_ARCH_UNKNOWN;
        const char* field = nullptr;
        if (line[0] == 'x' && line[1] == '6' && line[2] == '4' && line[3] == '_') {
            arch = GC_UPDATE_ARCH_X64;
            field = key + 4;
        } else if (line[0] == 'a' && line[1] == 'r' && line[2] == 'm' &&
                   line[3] == '6' && line[4] == '4' && line[5] == '_') {
            arch = GC_UPDATE_ARCH_ARM64;
            field = key + 6;
        }
        if (arch == GC_UPDATE_ARCH_UNKNOWN) {
            // Unknown keys are refused, not ignored.  See the header comment.
            gc_update_manifest_fail(out, "Unknown manifest key: ", key);
            return;
        }
        GcUpdateAsset* asset = arch == GC_UPDATE_ARCH_X64 ? &out->x64 : &out->arm64;
        int slot = arch == GC_UPDATE_ARCH_X64 ? 1 : 2;

        if (gc_update_text_equals(field, "file")) {
            if (sawFile[slot]) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawFile[slot] = true;
            if (!gc_update_copy_value(asset->file, sizeof(asset->file), value) ||
                !gc_update_asset_name_is_acceptable(asset->file)) {
                gc_update_manifest_fail(out, "Unacceptable asset name: ", value);
                return;
            }
        } else if (gc_update_text_equals(field, "size")) {
            if (sawSize[slot]) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawSize[slot] = true;
            if (!gc_update_parse_size(value, &asset->size)) {
                gc_update_manifest_fail(out, "Unacceptable asset size: ", value);
                return;
            }
        } else if (gc_update_text_equals(field, "sha256")) {
            if (sawHash[slot]) { gc_update_manifest_fail(out, "Duplicate key: ", key); return; }
            sawHash[slot] = true;
            if (!gc_update_parse_sha256(value, asset->sha256)) {
                gc_update_manifest_fail(out, "Unacceptable asset digest: ", value);
                return;
            }
        } else {
            gc_update_manifest_fail(out, "Unknown manifest key: ", key);
            return;
        }
    }

    if (!sawFormat) { gc_update_manifest_fail(out, "Manifest has no format key", nullptr); return; }
    if (!sawVersion) { gc_update_manifest_fail(out, "Manifest has no version key", nullptr); return; }

    // An architecture is either fully described or absent.  A partial triple is
    // a rejection of the whole document rather than of one slot: it means the
    // signer produced something it did not intend, and guessing which two of
    // the three fields to trust is not a decision this parser gets to make.
    for (int slot = 1; slot <= 2; ++slot) {
        GcUpdateArch arch = slot == 1 ? GC_UPDATE_ARCH_X64 : GC_UPDATE_ARCH_ARM64;
        GcUpdateAsset* asset = slot == 1 ? &out->x64 : &out->arm64;
        int count = (sawFile[slot] ? 1 : 0) + (sawSize[slot] ? 1 : 0) + (sawHash[slot] ? 1 : 0);
        if (count == 0) continue;
        if (count != 3) {
            gc_update_manifest_fail(out, "Incomplete asset entry for ",
                                    gc_update_arch_name(arch));
            return;
        }
        // The mix-and-match binding: the name must be the one this exact
        // version and architecture produces.
        char expected[GC_UPDATE_ASSET_NAME_MAX_CHARS];
        if (!gc_update_expected_asset_name(out->version.text, arch,
                                           expected, sizeof(expected)) ||
            !gc_update_text_equals(asset->file, expected)) {
            gc_update_manifest_fail(out, "Asset name does not match its version/arch: ",
                                    asset->file);
            return;
        }
        asset->present = true;
    }

    if (!out->x64.present && !out->arm64.present) {
        gc_update_manifest_fail(out, "Manifest describes no assets", nullptr);
        return;
    }
    out->valid = true;
    out->error[0] = 0;
}

static inline const GcUpdateAsset* gc_update_select_asset(
    const GcUpdateManifest* manifest, GcUpdateArch arch) {
    if (!manifest || !manifest->valid) return nullptr;
    if (arch == GC_UPDATE_ARCH_X64 && manifest->x64.present) return &manifest->x64;
    if (arch == GC_UPDATE_ARCH_ARM64 && manifest->arm64.present) return &manifest->arm64;
    return nullptr;
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

enum GcUpdateDecision {
    // The manifest did not parse, or named something not newer than what is
    // installed.  Both are refusals; they are separated below only so the log
    // can say which.
    GC_UPDATE_DECISION_REJECTED = 0,
    GC_UPDATE_DECISION_UP_TO_DATE = 1,
    GC_UPDATE_DECISION_AVAILABLE = 2,
    // Newer, but the installed build is below the release's declared floor.
    // The user is told to install by hand; a silent upgrade across that line
    // is what loses settings.
    GC_UPDATE_DECISION_MANUAL_REQUIRED = 3,
    // Newer, but this machine's architecture is not in the manifest.
    GC_UPDATE_DECISION_NO_ASSET = 4,
};

static inline GcUpdateDecision gc_update_decide(const GcUpdateManifest* manifest,
                                                const GcUpdateVersion* installed,
                                                GcUpdateArch arch) {
    if (!manifest || !manifest->valid) return GC_UPDATE_DECISION_REJECTED;
    if (!installed || !installed->valid) return GC_UPDATE_DECISION_REJECTED;
    if (arch == GC_UPDATE_ARCH_UNKNOWN) return GC_UPDATE_DECISION_REJECTED;

    // Downgrade gate first: an older release is not "up to date", it is a
    // replay, and it must never reach the asset or floor checks.
    int order = gc_update_version_compare(&manifest->version, installed);
    if (order < 0) return GC_UPDATE_DECISION_REJECTED;
    if (order == 0) return GC_UPDATE_DECISION_UP_TO_DATE;

    if (!gc_update_select_asset(manifest, arch)) return GC_UPDATE_DECISION_NO_ASSET;
    if (!gc_update_meets_minimum_from(installed,
                                      manifest->hasMinimumFrom ? &manifest->minimumFrom
                                                               : nullptr)) {
        return GC_UPDATE_DECISION_MANUAL_REQUIRED;
    }
    return GC_UPDATE_DECISION_AVAILABLE;
}

#endif // GREEN_CURVE_UPDATE_MANIFEST_POLICY_H
