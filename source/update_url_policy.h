// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Where the updater is allowed to talk, as pure logic.
//
// ## No GitHub API, and therefore no JSON parser
//
// The obvious design fetches `api.github.com/repos/.../releases/latest`, parses
// JSON, and reads the asset list out of it.  This does not do that, for three
// reasons that all point the same way:
//
//   - A JSON parser is a new untrusted-input boundary written to consume a
//     document an attacker controls the shape of.  The updater already has
//     exactly one parser it cannot avoid (the manifest), and that one runs
//     *after* a signature check.  A second one running *before* any signature
//     would be the least defended code in the feature.
//   - The unauthenticated API is rate limited to 60 requests/hour per IP.
//     Behind CGNAT or a corporate NAT that budget is shared, so a scheduled
//     check would fail for reasons the user cannot see or fix.
//   - `releases/latest/download/<name>` is a stable redirect to the newest
//     release's asset of that name, and GitHub's definition of "latest"
//     already excludes drafts and pre-releases.  That requirement is met by
//     the URL rather than by client-side filtering that could be wrong.
//
// So the manifest is uploaded to every release under a **version-independent**
// name, and the updater fetches exactly two fixed URLs.  The manifest declares
// the version; the asset URL is then built from a value that has already been
// signature-checked.
//
// ## The redirect allowlist
//
// `releases/latest/download/...` redirects, and the asset itself is served from
// a different host than the one the request started on.  Following redirects is
// therefore mandatory, which makes the redirect target an input an attacker
// would very much like to control -- a 302 to `http://` or to an arbitrary host
// turns "download from GitHub" into "download from anywhere".
//
// Every hop must independently be HTTPS, on the default port, on the host
// allowlist, and free of embedded credentials.  The chain is bounded so a
// redirect loop terminates as a refusal rather than as a hang.

#ifndef GREEN_CURVE_UPDATE_URL_POLICY_H
#define GREEN_CURVE_UPDATE_URL_POLICY_H

#include <stddef.h>

#include "update_manifest_policy.h"

#define GC_UPDATE_REPO_OWNER "aufkrawall"
#define GC_UPDATE_REPO_NAME  "green-curve"

// Version-independent names, uploaded to every release by `release.yml`.
// These must never contain the version: the whole point is that the updater
// can name them before it knows what the latest release is.
#define GC_UPDATE_MANIFEST_ASSET "greencurve-update-manifest.txt"
#define GC_UPDATE_SIGNATURE_ASSET "greencurve-update-manifest.sig"

#define GC_UPDATE_URL_MAX_CHARS 512
#define GC_UPDATE_HOST_MAX_CHARS 128
// Measured against the real chain: releases/latest/download -> the tag's
// download URL -> the object host.  Five leaves room for GitHub to add a hop
// without leaving room for a loop.
#define GC_UPDATE_MAX_REDIRECTS 5
// The signature is a fixed-size ECDSA P-256 value in base64; the manifest is a
// handful of lines.  Both caps exist so a hostile response cannot make the
// service read indefinitely before any check runs.
#define GC_UPDATE_SIGNATURE_MAX_BYTES 256

struct GcUpdateUrl {
    char host[GC_UPDATE_HOST_MAX_CHARS];
    char path[GC_UPDATE_URL_MAX_CHARS];
    bool valid;
};

// The only hosts the updater will connect to, on any hop.
static inline bool gc_update_host_is_allowed(const char* host) {
    if (!host || !host[0]) return false;
    static const char* const allowed[] = {
        "github.com",
        "objects.githubusercontent.com",
        "release-assets.githubusercontent.com",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (gc_update_text_equals(host, allowed[i])) return true;
    }
    return false;
}

// Parse `https://host/path`.  Deliberately narrow: no scheme other than
// https, no port (443 is implied and an explicit one is refused rather than
// compared, so there is one spelling), no userinfo, no fragment.
//
// Rejecting `user@host` matters more than it looks: a URL like
// `https://github.com@evil.example/...` has authority `evil.example`, and a
// naive "does it start with https://github.com" check accepts it.  The host is
// taken as the bytes after the last `@` in the authority precisely so that
// this parser cannot disagree with the HTTP stack about who is being called.
static inline void gc_update_url_parse(const char* url, GcUpdateUrl* out) {
    if (!out) return;
    GcUpdateUrl blank = {};
    *out = blank;
    if (!url) return;

    const char* scheme = "https://";
    size_t at = 0;
    for (; scheme[at]; ++at) {
        if (url[at] != scheme[at]) return;       // not HTTPS, or too short
    }

    // Authority runs to the first '/', '?' or '#'.
    size_t authorityStart = at;
    size_t cursor = at;
    while (url[cursor] && url[cursor] != '/' && url[cursor] != '?' &&
           url[cursor] != '#') {
        cursor++;
    }
    size_t authorityEnd = cursor;
    if (authorityEnd == authorityStart) return;  // empty authority

    for (size_t i = authorityStart; i < authorityEnd; ++i) {
        // Credentials, explicit ports and bracketed literals are all refused.
        if (url[i] == '@' || url[i] == ':' || url[i] == '[' || url[i] == ']') return;
    }

    size_t hostLength = authorityEnd - authorityStart;
    if (hostLength >= GC_UPDATE_HOST_MAX_CHARS) return;
    for (size_t i = 0; i < hostLength; ++i) {
        char c = url[authorityStart + i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
        // Uppercase is refused rather than folded, so the allowlist comparison
        // never needs a case table and there is one spelling of each host.
        if (!ok) return;
        out->host[i] = c;
    }
    out->host[hostLength] = 0;
    if (out->host[0] == '.' || out->host[0] == '-') return;
    if (out->host[hostLength - 1] == '.' || out->host[hostLength - 1] == '-') return;

    // A query is allowed through verbatim -- the object hosts sign their asset
    // URLs with one -- but a fragment is not: it is never sent on the wire, so
    // its presence means the caller and the stack disagree about the request.
    if (url[cursor] == '#') return;
    if (url[cursor] == 0) {
        out->path[0] = '/';
        out->path[1] = 0;
    } else {
        if (url[cursor] != '/') return;          // authority must end at a path
        size_t pathLength = 0;
        while (url[cursor]) {
            unsigned char c = (unsigned char)url[cursor];
            if (c < 0x20 || c == 0x7F || c == ' ') return;
            if (c == '#') return;
            if (pathLength + 1 >= GC_UPDATE_URL_MAX_CHARS) return;
            out->path[pathLength++] = (char)c;
            cursor++;
        }
        out->path[pathLength] = 0;
    }
    out->valid = true;
}

// The single gate every request and every redirect hop goes through.
static inline bool gc_update_url_is_acceptable(const char* url) {
    GcUpdateUrl parsed;
    gc_update_url_parse(url, &parsed);
    if (!parsed.valid) return false;
    return gc_update_host_is_allowed(parsed.host);
}

// A redirect is followed only when the hop count is still within budget AND
// the target passes the same gate the original request did.  Both halves are
// required: a bounded chain of hostile hops is still a chain of hostile hops.
static inline bool gc_update_redirect_is_acceptable(const char* targetUrl,
                                                    int hopsAlreadyFollowed) {
    if (hopsAlreadyFollowed < 0) return false;
    if (hopsAlreadyFollowed >= GC_UPDATE_MAX_REDIRECTS) return false;
    return gc_update_url_is_acceptable(targetUrl);
}

// ---------------------------------------------------------------------------
// URL construction
// ---------------------------------------------------------------------------

static inline bool gc_update_append(char* out, size_t outSize, size_t* at,
                                    const char* text) {
    if (!out || !at || !text) return false;
    for (size_t i = 0; text[i]; ++i) {
        if (*at + 1 >= outSize) return false;
        out[(*at)++] = text[i];
    }
    out[*at] = 0;
    return true;
}

// `https://github.com/<owner>/<repo>/releases/latest/download/<asset>`
//
// Resolves to the newest non-draft, non-prerelease release's asset of that
// name.  Both fetched URLs are built here rather than being configurable:
// a settings-file-supplied update URL is an obvious way to redirect the whole
// mechanism, and nothing about this feature needs one.
static inline bool gc_update_build_latest_url(const char* assetName,
                                              char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (!assetName || !gc_update_asset_name_is_acceptable(assetName)) return false;
    size_t at = 0;
    if (!gc_update_append(out, outSize, &at, "https://github.com/")) return false;
    if (!gc_update_append(out, outSize, &at, GC_UPDATE_REPO_OWNER)) return false;
    if (!gc_update_append(out, outSize, &at, "/")) return false;
    if (!gc_update_append(out, outSize, &at, GC_UPDATE_REPO_NAME)) return false;
    if (!gc_update_append(out, outSize, &at, "/releases/latest/download/")) return false;
    if (!gc_update_append(out, outSize, &at, assetName)) return false;
    return gc_update_url_is_acceptable(out);
}

// `https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>`
//
// The tag and the asset name both come from a manifest that has ALREADY passed
// signature verification, which is why this function is safe to build a URL
// from at all.  It re-validates both anyway: the cost is nothing and the
// alternative is that a future caller reorders the steps and nobody notices.
static inline bool gc_update_build_asset_url(const char* versionText,
                                             const char* assetName,
                                             char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    GcUpdateVersion version;
    gc_update_version_parse(versionText, &version);
    if (!version.valid) return false;
    if (!assetName || !gc_update_asset_name_is_acceptable(assetName)) return false;

    size_t at = 0;
    if (!gc_update_append(out, outSize, &at, "https://github.com/")) return false;
    if (!gc_update_append(out, outSize, &at, GC_UPDATE_REPO_OWNER)) return false;
    if (!gc_update_append(out, outSize, &at, "/")) return false;
    if (!gc_update_append(out, outSize, &at, GC_UPDATE_REPO_NAME)) return false;
    if (!gc_update_append(out, outSize, &at, "/releases/download/")) return false;
    if (!gc_update_append(out, outSize, &at, version.text)) return false;
    if (!gc_update_append(out, outSize, &at, "/")) return false;
    if (!gc_update_append(out, outSize, &at, assetName)) return false;
    return gc_update_url_is_acceptable(out);
}

#endif // GREEN_CURVE_UPDATE_URL_POLICY_H
