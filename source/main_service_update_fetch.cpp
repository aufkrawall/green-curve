// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The updater's only outbound network path, over WinHTTP.
//
// ## Redirects are followed by hand, on purpose
//
// `releases/latest/download/<name>` is a redirect chain -- it resolves to the
// newest release's tag URL, which resolves again to an object host.  Following
// redirects is therefore mandatory, which makes the redirect target an input an
// attacker would very much like to control: a single 302 to `http://` or to an
// arbitrary host turns "download from GitHub" into "download from anywhere".
//
// WinHTTP's automatic redirect handling is switched OFF and each hop is
// re-validated through `gc_update_redirect_is_acceptable()`, which requires
// HTTPS, the default port, an allowlisted host, no embedded credentials, and a
// hop count still inside its budget.  Letting WinHTTP follow them and checking
// only the final URL would be too late: the request, including any headers,
// would already have been sent to whatever host the chain pointed at.
//
// ## What this code deliberately does not do
//
//   - It never disables certificate or revocation checking.  There is no
//     "ignore certificate errors" flag anywhere in this file, and TLS is
//     pinned to 1.2+ rather than left at whatever the OS default happens to be.
//   - It sends no credentials and accepts no cookies.  Everything fetched is a
//     public release asset; an updater that could authenticate would be an
//     updater whose credentials could be stolen.
//   - It never trusts a Content-Length.  Reads are bounded by the caller's own
//     ceiling, so a hostile or broken server cannot make the service read
//     indefinitely, and the download stops the moment it exceeds what the
//     signed manifest said the file should be.

// WinHTTP rather than WinINet: WinINet is explicitly unsupported from a
// service, and WinHTTP lets automatic redirect handling be switched off so each
// hop can be validated before the next request is sent.  Included here rather
// than in main.cpp so the dependency sits with the only code that uses it.
#include <winhttp.h>
// Cross-session process enumeration for the pre-install GUI stop in
// main_service_update_worker.cpp.  Included from the first updater shard so it
// lands ahead of every consumer in the amalgamation.
#include <tlhelp32.h>

#define GC_UPDATE_HTTP_TIMEOUT_MS 30000
#define GC_UPDATE_HTTP_USER_AGENT L"GreenCurve-Updater"

struct GcUpdateHttpHandles {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
};

static void gc_update_http_close(GcUpdateHttpHandles* handles) {
    if (!handles) return;
    if (handles->request) { WinHttpCloseHandle(handles->request); handles->request = nullptr; }
    if (handles->connect) { WinHttpCloseHandle(handles->connect); handles->connect = nullptr; }
    if (handles->session) { WinHttpCloseHandle(handles->session); handles->session = nullptr; }
}

// Read the Location header of a redirect response as UTF-8.
static bool gc_update_http_read_location(HINTERNET request, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    // A Location header longer than any URL we could store is refused rather
    // than truncated: a truncated URL could parse as a different, allowlisted
    // one, which is a far worse outcome than a failed update.
    //
    // `bytes` counts UTF-16 including the terminator, so the ceiling is the
    // UTF-8 buffer size doubled.  Sizing this too tightly is not hypothetical --
    // GitHub's signed asset URLs are ~945 characters and a 512-char limit
    // rejected every real redirect (see GC_UPDATE_URL_MAX_CHARS).
    if (bytes > (DWORD)(GC_UPDATE_URL_MAX_CHARS * sizeof(WCHAR))) return false;

    WCHAR* wide = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + sizeof(WCHAR));
    if (!wide) return false;
    bool ok = false;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            wide, &bytes, WINHTTP_NO_HEADER_INDEX)) {
        int converted = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)outSize,
                                            nullptr, nullptr);
        ok = converted > 0;
        if (!ok) out[0] = 0;
    }
    HeapFree(GetProcessHeap(), 0, wide);
    return ok;
}

// Issue a GET and follow allowlisted redirects until a 200 arrives.  On success
// `handles->request` is open and positioned to be read.
static bool gc_update_http_open(const char* url, GcUpdateHttpHandles* handles,
                                char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!handles) return false;
    GcUpdateHttpHandles blank = {};
    *handles = blank;

    char current[GC_UPDATE_URL_MAX_CHARS] = {};
    if (!url || FAILED(StringCchCopyA(current, sizeof(current), url))) {
        StringCchCopyA(err, errSize, "update URL is missing or too long");
        return false;
    }
    // The very first URL goes through the same gate every redirect does.  It is
    // built by update_url_policy.h from compiled-in constants, so this can only
    // fail if that construction is wrong -- which is exactly when it should.
    if (!gc_update_url_is_acceptable(current)) {
        StringCchCopyA(err, errSize, "update URL failed the host allowlist");
        return false;
    }

    handles->session = WinHttpOpen(GC_UPDATE_HTTP_USER_AGENT,
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handles->session) {
        StringCchPrintfA(err, errSize, "cannot open an HTTP session (error %lu)",
                         GetLastError());
        return false;
    }
    WinHttpSetTimeouts(handles->session, GC_UPDATE_HTTP_TIMEOUT_MS,
                       GC_UPDATE_HTTP_TIMEOUT_MS, GC_UPDATE_HTTP_TIMEOUT_MS,
                       GC_UPDATE_HTTP_TIMEOUT_MS);
    // TLS 1.2+ explicitly.  Left at the OS default this would silently follow
    // whatever an old or policy-modified system still enables.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(handles->session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &protocols, sizeof(protocols));

    for (int hop = 0; hop <= GC_UPDATE_MAX_REDIRECTS; ++hop) {
        GcUpdateUrl parsed;
        gc_update_url_parse(current, &parsed);
        if (!parsed.valid || !gc_update_host_is_allowed(parsed.host)) {
            StringCchCopyA(err, errSize, "refused a URL outside the host allowlist");
            gc_update_http_close(handles);
            return false;
        }

        GcWideUtf8Arg wideHost(parsed.host);
        GcWideUtf8Arg widePath(parsed.path);
        if (!wideHost.valid_for(parsed.host) || !widePath.valid_for(parsed.path)) {
            StringCchCopyA(err, errSize, "cannot encode the update URL");
            gc_update_http_close(handles);
            return false;
        }

        if (handles->request) { WinHttpCloseHandle(handles->request); handles->request = nullptr; }
        if (handles->connect) { WinHttpCloseHandle(handles->connect); handles->connect = nullptr; }

        handles->connect = WinHttpConnect(handles->session, wideHost.value,
                                          INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!handles->connect) {
            StringCchPrintfA(err, errSize, "cannot connect to %s (error %lu)",
                             parsed.host, GetLastError());
            gc_update_http_close(handles);
            return false;
        }
        handles->request = WinHttpOpenRequest(handles->connect, L"GET", widePath.value,
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE);
        if (!handles->request) {
            StringCchPrintfA(err, errSize, "cannot build the request (error %lu)",
                             GetLastError());
            gc_update_http_close(handles);
            return false;
        }

        // Redirects are ours to validate, so WinHTTP must not chase them.
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(handles->request, WINHTTP_OPTION_DISABLE_FEATURE,
                         &disable, sizeof(disable));

        if (!WinHttpSendRequest(handles->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(handles->request, nullptr)) {
            DWORD error = GetLastError();
            // ERROR_WINHTTP_SECURE_FAILURE is reported as itself rather than
            // softened: a TLS failure is the one network error that must never
            // read as "try again later".
            StringCchPrintfA(err, errSize,
                             error == ERROR_WINHTTP_SECURE_FAILURE
                                 ? "TLS verification failed for %s (error %lu)"
                                 : "request to %s failed (error %lu)",
                             parsed.host, error);
            gc_update_http_close(handles);
            return false;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(handles->request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            StringCchCopyA(err, errSize, "no HTTP status in the response");
            gc_update_http_close(handles);
            return false;
        }

        if (status == 200) {
            debug_log("update fetch: 200 from %s after %d redirect(s)\n",
                      parsed.host, hop);
            return true;
        }
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            char location[GC_UPDATE_URL_MAX_CHARS] = {};
            if (!gc_update_http_read_location(handles->request, location, sizeof(location))) {
                StringCchCopyA(err, errSize, "redirect without a usable Location header");
                gc_update_http_close(handles);
                return false;
            }
            if (!gc_update_redirect_is_acceptable(location, hop)) {
                // The refusal names the hop but not the target: echoing an
                // attacker-chosen URL into a log and a GUI status line is a
                // small gift there is no reason to give.
                StringCchPrintfA(err, errSize,
                                 "refused a redirect at hop %d (off-allowlist, "
                                 "not HTTPS, or past the redirect budget)", hop);
                debug_log("update fetch: REFUSED redirect hop=%d from host=%s\n",
                          hop, parsed.host);
                gc_update_http_close(handles);
                return false;
            }
            StringCchCopyA(current, sizeof(current), location);
            continue;
        }
        if (status == 403 || status == 429) {
            // Reported distinctly so the scheduler's backoff can be understood
            // from a log rather than looking like a generic failure loop.
            StringCchPrintfA(err, errSize, "GitHub rate limited the update check (HTTP %lu)",
                             (unsigned long)status);
            gc_update_http_close(handles);
            return false;
        }
        if (status == 404) {
            StringCchCopyA(err, errSize, "no published update manifest was found");
            gc_update_http_close(handles);
            return false;
        }
        StringCchPrintfA(err, errSize, "unexpected HTTP status %lu", (unsigned long)status);
        gc_update_http_close(handles);
        return false;
    }

    StringCchCopyA(err, errSize, "too many redirects");
    gc_update_http_close(handles);
    return false;
}

// ---------------------------------------------------------------------------
// The transport seam
// ---------------------------------------------------------------------------
//
// The two read loops live in update_transport_policy.h so the mid-transfer size
// abort can be asserted; these adapters are all that is left of them here.  The
// pairing is one-to-one with the WinHTTP calls they replaced, so nothing about
// the sequence changed -- see that header for why a faithful transcription
// rather than a nicer interface.

static bool gc_update_winhttp_available(void* ctx, size_t* bytes) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable((HINTERNET)ctx, &available)) return false;
    if (bytes) *bytes = (size_t)available;
    return true;
}

static bool gc_update_winhttp_read(void* ctx, void* buffer, size_t want, size_t* got) {
    // WinHttpReadData takes a DWORD.  Both callers already clamp -- the
    // document reader by its buffer, the asset streamer by its chunk -- so this
    // clamp is a belt-and-braces guard for a 64-bit size_t rather than a
    // reachable path.
    DWORD wanted = want > 0xFFFFFFFFull ? 0xFFFFFFFFul : (DWORD)want;
    DWORD read = 0;
    if (!WinHttpReadData((HINTERNET)ctx, buffer, wanted, &read)) return false;
    if (got) *got = (size_t)read;
    return true;
}

static bool gc_update_file_sink_write(void* ctx, const void* buffer, size_t bytes) {
    DWORD written = 0;
    if (!WriteFile((HANDLE)ctx, buffer, (DWORD)bytes, &written, nullptr)) return false;
    return written == (DWORD)bytes;
}

// Turn a pure result back into the sentence this file has always produced.
// GetLastError() is read here because the pure loops cannot know it, and it is
// read immediately after the failing call for the same reason.
static void gc_update_fetch_describe(GcUpdateFetchResult result, char* err, size_t errSize) {
    switch (result) {
        case GC_UPDATE_FETCH_QUERY_FAILED:
            StringCchPrintfA(err, errSize, "download failed (error %lu)", GetLastError());
            return;
        case GC_UPDATE_FETCH_READ_FAILED:
            StringCchPrintfA(err, errSize, "download stalled (error %lu)", GetLastError());
            return;
        case GC_UPDATE_FETCH_TOO_LARGE:
            StringCchCopyA(err, errSize, "the fetched document is larger than allowed");
            return;
        case GC_UPDATE_FETCH_EMPTY:
            StringCchCopyA(err, errSize, "the server returned an empty document");
            return;
        case GC_UPDATE_FETCH_WRITE_FAILED:
            StringCchPrintfA(err, errSize, "cannot write the staged file (error %lu)",
                             GetLastError());
            return;
        case GC_UPDATE_FETCH_BAD_EXPECTED_SIZE:
            StringCchCopyA(err, errSize, "manifest size is outside the allowed range");
            return;
        case GC_UPDATE_FETCH_BAD_ARGUMENTS:
            StringCchCopyA(err, errSize, "invalid download arguments");
            return;
        case GC_UPDATE_FETCH_SIZE_MISMATCH:
        case GC_UPDATE_FETCH_OK:
            // SIZE_MISMATCH names both numbers, so its caller writes it.
            return;
    }
}

// Fetch a small document (the manifest or its signature) into a caller buffer.
// The ceiling is the caller's, never the server's Content-Length.
static bool gc_update_http_get_small(const char* url, char* out, size_t outSize,
                                     size_t* outLen, char* err, size_t errSize) {
    if (outLen) *outLen = 0;
    if (!out || outSize == 0) return false;
    out[0] = 0;

    GcUpdateHttpHandles handles = {};
    if (!gc_update_http_open(url, &handles, err, errSize)) return false;

    GcUpdateReader reader = {};
    reader.available = gc_update_winhttp_available;
    reader.read = gc_update_winhttp_read;
    reader.ctx = handles.request;

    GcUpdateFetchResult result = gc_update_read_document(&reader, out, outSize, outLen);
    if (result != GC_UPDATE_FETCH_OK) gc_update_fetch_describe(result, err, errSize);
    gc_update_http_close(&handles);
    return result == GC_UPDATE_FETCH_OK;
}

// Stream an asset into an already-open destination handle.
//
// `expectedBytes` comes from the signature-verified manifest, and the transfer
// is aborted the moment it is exceeded rather than after the fact: the point of
// the ceiling is to bound what a hostile response can make the service write to
// disk, which is only useful if it stops the write in progress.  A short file is
// caught by the equality check at the end, and by the digest afterwards.
static bool gc_update_http_download_to_handle(const char* url, HANDLE dest,
                                              unsigned long long expectedBytes,
                                              char* err, size_t errSize) {
    if (!dest || dest == INVALID_HANDLE_VALUE) {
        StringCchCopyA(err, errSize, "no destination handle");
        return false;
    }

    GcUpdateHttpHandles handles = {};
    if (!gc_update_http_open(url, &handles, err, errSize)) return false;

    static constexpr size_t kChunk = static_cast<size_t>(256) * 1024;
    unsigned char* buffer = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, kChunk);
    if (!buffer) {
        StringCchCopyA(err, errSize, "out of memory downloading the update");
        gc_update_http_close(&handles);
        return false;
    }

    GcUpdateReader reader = {};
    reader.available = gc_update_winhttp_available;
    reader.read = gc_update_winhttp_read;
    reader.ctx = handles.request;
    GcUpdateSink sink = {};
    sink.write = gc_update_file_sink_write;
    sink.ctx = dest;

    unsigned long long total = 0;
    GcUpdateFetchResult result = gc_update_stream_asset(
        &reader, &sink, expectedBytes, buffer, kChunk, &total);

    HeapFree(GetProcessHeap(), 0, buffer);
    gc_update_http_close(&handles);

    if (result == GC_UPDATE_FETCH_TOO_LARGE) {
        // Reported from here rather than from the loop because the loop is the
        // one thing that must not stop to format a string: it has already
        // refused, and it refused BEFORE the overflowing chunk reached disk.
        StringCchCopyA(err, errSize,
                       "the download is larger than the signed manifest says");
        debug_log("update fetch: ABORTED oversized download past %llu bytes "
                  "(manifest says %llu)\n", total, expectedBytes);
        return false;
    }
    if (result == GC_UPDATE_FETCH_SIZE_MISMATCH) {
        StringCchPrintfA(err, errSize,
                         "downloaded %llu bytes; the signed manifest says %llu",
                         total, expectedBytes);
        return false;
    }
    if (result != GC_UPDATE_FETCH_OK) {
        gc_update_fetch_describe(result, err, errSize);
        return false;
    }

    // Flush before the digest is taken, so the verification reads what is
    // actually on disk rather than what is still sitting in a cache.
    FlushFileBuffers(dest);
    debug_log("update fetch: downloaded %llu bytes\n", total);
    return true;
}
