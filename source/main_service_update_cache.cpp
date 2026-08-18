// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Carrying the last check's RESULT across a service restart.
//
// ## The problem this fixes
//
// Everything the last check learned -- the manifest, the decision, the staged
// and verified package -- lived only in `GcUpdateRuntimeState`, which is
// process memory.  Only the POLICY was persisted (auto_check, interval,
// consecutive_failures, last_check).  So a reboot, an SCM restart or a
// controlled recovery restart reset `decision` to `REJECTED` and
// `manifestValid` to false, while `lastCheckUnix` survived and kept the next
// automatic check up to a full interval away.
//
// The user-visible result: an update found at 10:00 and staged, a reboot at
// 10:05, and a machine that shows no update anywhere -- no tray entry, no
// orange Updates button, a dialog reading "No update information available" --
// until 10:00 the NEXT DAY.  On a machine that is rebooted daily, an update
// could stay unadvertised indefinitely.  The verified installer meanwhile sat
// orphaned in the staging directory and was deleted and re-downloaded by the
// next check, because nothing knew it was there.
//
// ## Why the MANIFEST is cached and not the decision
//
// Caching "decision = AVAILABLE, version = 0.30" would be caching a conclusion,
// and reloading it would mean trusting a file instead of a signature.  Anything
// with write access to the machine config directory could then make the GUI
// advertise a version that was never published -- and the install path would
// still refuse it, so the user would be left with a badge they cannot act on.
//
// So the cache stores the two documents the check itself fetched, byte for
// byte, and the restore runs THE SAME GATE in THE SAME ORDER:
//
//   1. verify the detached signature over the manifest's exact bytes;
//   2. only then parse;
//   3. only then decide, against the version this binary is actually running.
//
// Every invariant in updates.md therefore holds identically for a cached
// manifest and a fetched one.  In particular a replayed older release is
// refused by `gc_update_decide()` here exactly as it is on the network path,
// and the decision is recomputed rather than restored -- which is what makes
// the cache self-correcting after an update actually installs: the same cached
// manifest that said AVAILABLE for 0.23.1 says UP_TO_DATE once 0.30 is running.
//
// ## Why not atomic, and why that is fine
//
// The two files are written back to back after a successful check.  A crash
// between them leaves a new manifest beside an old signature, which fails
// verification -- and a failed restore deletes both and continues exactly as if
// no cache existed.  Every torn state is therefore fail-closed into "no cache",
// so there is nothing for atomicity to buy.  The writes still go through the
// service writer because it refuses a reparse-pointed parent and re-checks
// where the handle actually landed, which is worth having on a path the service
// writes as SYSTEM.
//
// They go through it with GC_SERVICE_WRITE_MACHINE_CONFIG, and that argument is
// load-bearing.  The default scope confines a write to the *calling client's*
// profile, which is correct for a path a client named and wrong for this one:
// %ProgramData%\Green Curve is inside no user's profile, so every cache write
// was refused with "Path is outside the caller's profile directory" from the
// day the cache shipped until 2026-08-17.  Nothing surfaced -- the restore path
// is written to treat a missing cache as the ordinary state of a machine that
// has never checked, so a cache that never stored was indistinguishable from a
// cache that was never needed.  The machine scope is a different containment
// root, not an absent one: the write must still land inside the protected
// machine config directory.
//
// ## Where the files live
//
// Beside `shared-profiles.ini` in `%ProgramData%\Green Curve`, NOT in the
// staging directory -- `service_update_clear_staging()` deletes every file in
// there before each download, and a cache that a download erases is not a
// cache.  The directory's DACL (SYSTEM + Administrators full, Users read-only)
// is the same one that protects the staged installer, and is re-applied to each
// file for the same reason `service_update_save_settings()` re-applies it.

// Names live in machine_dir_policy.h, shared with the legacy sweeper that
// would otherwise delete these files on every service start.

static bool service_update_cache_path(const char* leafName, char* out, size_t outSize) {
    if (out && outSize) out[0] = 0;
    if (!leafName || !leafName[0]) return false;
    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) return false;
    char* slash = strrchr(machinePath, '\\');
    if (!slash) return false;
    *slash = 0;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\%s", machinePath, leafName));
}

static void service_update_cache_clear() {
    char manifestPath[MAX_PATH] = {};
    char signaturePath[MAX_PATH] = {};
    int removed = 0;
    if (service_update_cache_path(GC_UPDATE_CACHE_MANIFEST_NAME, manifestPath,
                                  sizeof(manifestPath)) &&
        gc_DeleteFileUtf8(manifestPath)) {
        removed++;
    }
    if (service_update_cache_path(GC_UPDATE_CACHE_SIGNATURE_NAME, signaturePath,
                                  sizeof(signaturePath)) &&
        gc_DeleteFileUtf8(signaturePath)) {
        removed++;
    }
    if (removed) debug_log("update cache: removed %d cached document(s)\n", removed);
}

// Read a whole small file.  The caller's buffer bound IS the size limit: both
// documents have a format maximum (4096 / 256 bytes) and anything larger is not
// a truncated version of a valid one, it is a different file.
static bool service_update_cache_read(const char* path, char* out, size_t outSize,
                                      size_t* lengthOut) {
    if (lengthOut) *lengthOut = 0;
    if (!path || !path[0] || !out || outSize < 2) return false;
    out[0] = 0;

    HANDLE file = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        // A missing cache is the ordinary state on a machine that has never
        // completed a check; anything else is worth a line in the log.
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            debug_log("update cache: cannot open %s (error %lu)\n", path, error);
        }
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        (unsigned long long)size.QuadPart > (unsigned long long)(outSize - 1)) {
        debug_log("update cache: %s has an unusable size (%lld bytes, max %zu)\n",
                  path, (long long)size.QuadPart, outSize - 1);
        CloseHandle(file);
        return false;
    }
    DWORD wanted = (DWORD)size.QuadPart;
    DWORD read = 0;
    BOOL ok = ReadFile(file, out, wanted, &read, nullptr);
    CloseHandle(file);
    if (!ok || read != wanted) {
        debug_log("update cache: short read on %s (%lu of %lu bytes)\n",
                  path, (unsigned long)read, (unsigned long)wanted);
        return false;
    }
    out[read] = 0;
    if (lengthOut) *lengthOut = (size_t)read;
    return true;
}

static bool service_update_cache_write_one(const char* path, const char* data,
                                           size_t dataSize) {
    char err[256] = {};
    if (!write_text_file_atomic_service_scoped(path, data, dataSize,
                                               GC_SERVICE_WRITE_MACHINE_CONFIG,
                                               err, sizeof(err))) {
        debug_log("update cache: cannot write %s: %s\n", path,
                  err[0] ? err : "unknown");
        return false;
    }
    debug_log("update cache: wrote %s (%zu bytes)\n", path, dataSize);
    Win32Utf8Path widePath(path);
    if (widePath.valid_for(path)) {
        char aclErr[256] = {};
        if (!apply_protected_machine_config_dacl(widePath.value, aclErr, sizeof(aclErr))) {
            debug_log("update cache: DACL hardening failed for %s: %s\n", path,
                      aclErr[0] ? aclErr : "unknown");
        }
    }
    return true;
}

// Store the two documents the check just authenticated.  Called ONLY after
// `gc_update_verify_manifest_signature()` succeeded, so what lands on disk is
// bytes that verified -- and the restore verifies them again anyway, because
// the file is what an attacker with write access would target and "we wrote it
// ourselves last boot" is not a security property.
static void service_update_cache_store(const char* manifestText, size_t manifestLength,
                                       const char* signatureText, size_t signatureLength) {
    if (!manifestText || !manifestLength || !signatureText || !signatureLength) return;

    char err[256] = {};
    if (!ensure_machine_config_directory(err, sizeof(err))) {
        debug_log("update cache: cannot prepare the machine config directory: %s\n",
                  err[0] ? err : "unknown");
        return;
    }
    char manifestPath[MAX_PATH] = {};
    char signaturePath[MAX_PATH] = {};
    if (!service_update_cache_path(GC_UPDATE_CACHE_MANIFEST_NAME, manifestPath,
                                   sizeof(manifestPath)) ||
        !service_update_cache_path(GC_UPDATE_CACHE_SIGNATURE_NAME, signaturePath,
                                   sizeof(signaturePath))) {
        debug_log("update cache: cannot resolve the cache paths\n");
        return;
    }
    // Manifest first, signature second.  The order matters only for the crash
    // window, and this is the direction whose torn state (new manifest, old
    // signature) cannot verify -- the reverse could pair an OLD manifest with a
    // signature that genuinely covers it, silently pinning the cache a release
    // behind until the next successful check.
    if (!service_update_cache_write_one(manifestPath, manifestText, manifestLength) ||
        !service_update_cache_write_one(signaturePath, signatureText, signatureLength)) {
        service_update_cache_clear();
        return;
    }
    debug_log("update cache: stored the verified manifest (%zu bytes) and signature (%zu bytes)\n",
              manifestLength, signatureLength);
}

// Re-adopt a package left staged by a previous process.
//
// The name is not read from disk: it is rebuilt from the manifest that was just
// verified, so a file the sweep missed under some other name can never be
// picked up. `service_update_staged_package_matches_manifest()` then re-hashes
// it through a write-and-delete-denying handle, exactly as the download did.
// The install path re-verifies AGAIN through a freshly pinned handle before
// CreateProcessW, so this is a "do we need to download it again?" question, not
// a trust decision.
static void service_update_adopt_staged_package(const GcUpdateManifest* manifest) {
    if (!manifest || !manifest->valid) return;
    const GcUpdateAsset* asset =
        gc_update_select_asset(manifest, service_update_host_arch());
    if (!asset || !asset->file[0]) return;

    char err[256] = {};
    char stagingDir[MAX_PATH] = {};
    if (!service_update_staging_dir(stagingDir, sizeof(stagingDir), err, sizeof(err))) {
        debug_log("update cache: no staging directory to adopt from: %s\n",
                  err[0] ? err : "unknown");
        return;
    }
    char stagedPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(stagedPath, sizeof(stagedPath), "%s\\%s",
                                stagingDir, asset->file))) {
        return;
    }
    DWORD attrs = gc_GetFileAttributesUtf8(stagedPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        // Nothing staged is the normal case: the check ran but the download had
        // not happened, or the install already consumed it.
        return;
    }

    {
        GcUpdateStateLock guard;
        StringCchCopyA(g_updateState.stagedPath, sizeof(g_updateState.stagedPath),
                       stagedPath);
    }
    if (!service_update_staged_package_matches_manifest(manifest, err, sizeof(err))) {
        debug_log("update cache: staged package does not match the cached manifest (%s); discarding it\n",
                  err[0] ? err : "no reason given");
        service_update_clear_staging();
        return;
    }
    {
        GcUpdateStateLock guard;
        g_updateState.packageStaged = true;
        g_updateState.packageVerified = true;
    }
    service_update_set_phase(SERVICE_UPDATE_PHASE_READY, nullptr);
    debug_log("update cache: re-adopted the staged package %s (no re-download needed)\n",
              asset->file);
}

// Called once at service start, after the machine config directory has been
// hardened and the policy loaded.  Runs synchronously: the manifest is at most
// 4 KB, the signature 256 bytes, and the only expensive step -- hashing the
// staged installer, under a megabyte -- happens only when one is actually
// there, which is the rare case.
static void service_update_restore_from_cache() {
    char manifestPath[MAX_PATH] = {};
    char signaturePath[MAX_PATH] = {};
    if (!service_update_cache_path(GC_UPDATE_CACHE_MANIFEST_NAME, manifestPath,
                                   sizeof(manifestPath)) ||
        !service_update_cache_path(GC_UPDATE_CACHE_SIGNATURE_NAME, signaturePath,
                                   sizeof(signaturePath))) {
        return;
    }

    char manifestText[GC_UPDATE_MANIFEST_MAX_BYTES + 1] = {};
    size_t manifestLength = 0;
    char signatureText[GC_UPDATE_SIGNATURE_MAX_BYTES + 1] = {};
    size_t signatureLength = 0;
    if (!service_update_cache_read(manifestPath, manifestText, sizeof(manifestText),
                                   &manifestLength) ||
        !service_update_cache_read(signaturePath, signatureText, sizeof(signatureText),
                                   &signatureLength)) {
        return;
    }

    // THE SAME GATE, in the same order as the network path: verify the
    // signature over the exact bytes, and only then let the parser see them.
    char err[256] = {};
    if (!gc_update_verify_manifest_signature(manifestText, manifestLength,
                                             signatureText, signatureLength,
                                             err, sizeof(err))) {
        debug_log("update cache: REFUSED the cached manifest: %s; discarding it\n",
                  err[0] ? err : "signature verification failed");
        service_update_cache_clear();
        return;
    }

    GcUpdateManifest manifest;
    gc_update_manifest_parse(manifestText, manifestLength, &manifest);
    if (!manifest.valid) {
        debug_log("update cache: cached manifest does not parse (%s); discarding it\n",
                  manifest.error[0] ? manifest.error : "unknown");
        service_update_cache_clear();
        return;
    }

    GcUpdateVersion installed;
    gc_update_version_parse(APP_VERSION, &installed);
    if (!installed.valid) {
        debug_log("update cache: this build's version (%s) is not a release version; ignoring the cache\n",
                  APP_VERSION);
        return;
    }

    GcUpdateArch arch = service_update_host_arch();
    // Recomputed, never restored.  This is what makes the cache correct across
    // an install: the manifest that advertised 0.30 to a 0.23.1 machine
    // answers UP_TO_DATE once 0.30 is the running binary.
    GcUpdateDecision decision = gc_update_decide(&manifest, &installed, arch);
    {
        GcUpdateStateLock guard;
        g_updateState.manifest = manifest;
        g_updateState.manifestValid = true;
        g_updateState.decision = decision;
    }
    debug_log("update cache: restored decision=%d published=%s installed=%s arch=%s\n",
              (int)decision, manifest.version.text, installed.text,
              gc_update_arch_name(arch));

    if (decision == GC_UPDATE_DECISION_AVAILABLE) {
        service_update_adopt_staged_package(&manifest);
        return;
    }
    // Anything else means nothing staged could be wanted: an installer for the
    // version already running, or for a release this machine has no build for,
    // is dead weight in a directory the service launches things from.
    service_update_clear_staging();
}
