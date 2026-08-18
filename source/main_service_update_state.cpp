// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The updater's service-side state: what the last check found, where the
// staged package lives, and the persisted policy.
//
// ## The settings are machine scope, not per user
//
// One machine has one installation, one service and therefore one update
// policy.  Putting the toggle in each user's config.ini would let two accounts
// disagree about whether a shared service may make outbound requests, and the
// service would have to pick a winner -- which is a question with no defensible
// answer.  So it lives beside the shared profile bank in
// %ProgramData%\Green Curve, whose directory is already created and hardened
// (SYSTEM + Administrators full, Users read-only) by
// ensure_machine_config_directory().
//
// That DACL is also exactly what the staging directory needs, and it is the
// reason the download lands there rather than in a temp folder: the file the
// service is about to execute as SYSTEM must not be writable by the
// unprivileged account that asked for the update.  Staging into the invoking
// user's %TEMP% and then elevating is the classic form of this bug, and the
// installer's own design notes already refuse it for the same reason.

// The wire struct spells its version buffer as a literal so service_protocol.h
// can stay include-free.  If the policy's bound ever moves, this is where the
// two are held together.
static_assert(SERVICE_UPDATE_VERSION_CHARS == GC_UPDATE_VERSION_MAX_CHARS,
              "wire version buffer must match update_version_policy.h");
static_assert(sizeof(((ServiceUpdateState*)nullptr)->detail) >= 128,
              "update detail buffer is too small to carry a useful reason");

#define GC_UPDATE_CONFIG_SECTION "updates"
#define GC_UPDATE_STAGING_DIR_NAME "updates"

struct GcUpdateRuntimeState {
    CRITICAL_SECTION lock;
    bool lockReady;

    ServiceUpdatePhase phase;
    GcUpdateDecision decision;
    GcUpdateAutoCheck autoCheck;
    int intervalSeconds;
    int consecutiveFailures;
    GcUpdateInstallRefusal lastRefusal;
    long long lastCheckUnix;

    // The manifest from the last successful check.  Only ever written after its
    // signature verified; a failed check leaves the previous one alone rather
    // than clearing it, so a transient network problem does not make a
    // already-staged update disappear from the GUI.
    GcUpdateManifest manifest;
    bool manifestValid;
    // Highest version ever advertised by a signature-verified manifest.
    // Persisted, because the whole point is to notice a channel that goes
    // backwards across restarts. See update_channel_policy.h.
    char highestSeenVersion[GC_UPDATE_VERSION_MAX_CHARS];

    char stagedPath[MAX_PATH];
    bool packageStaged;
    bool packageVerified;
    bool installRunning;
    bool workerRunning;
    bool guiShutdownRequested;
    // The authenticated pipe session that requested this install.  Setup runs
    // in session 0, so this is the only safe way to tell it which interactive
    // token should receive the relaunched GUI.
    DWORD requestingSessionId;

    char detail[256];
};

static GcUpdateRuntimeState g_updateState = {};

static void service_update_state_init() {
    if (g_updateState.lockReady) return;
    InitializeCriticalSection(&g_updateState.lock);
    g_updateState.lockReady = true;
    g_updateState.phase = SERVICE_UPDATE_PHASE_IDLE;
    g_updateState.decision = GC_UPDATE_DECISION_REJECTED;
    g_updateState.autoCheck = GC_UPDATE_AUTO_CHECK_UNSET;
    g_updateState.intervalSeconds = GC_UPDATE_INTERVAL_DEFAULT_SECONDS;
    g_updateState.lastRefusal = GC_UPDATE_INSTALL_ALLOWED;
    g_updateState.requestingSessionId = (DWORD)-1;
}

// Every mutation goes through the lock: the pipe thread reads this state to
// answer GET_UPDATE_STATE while the worker thread is writing it.
struct GcUpdateStateLock {
    GcUpdateStateLock() {
        service_update_state_init();
        EnterCriticalSection(&g_updateState.lock);
    }
    ~GcUpdateStateLock() { LeaveCriticalSection(&g_updateState.lock); }
    GcUpdateStateLock(const GcUpdateStateLock&) = delete;
    GcUpdateStateLock& operator=(const GcUpdateStateLock&) = delete;
};

static void service_update_set_phase(ServiceUpdatePhase phase, const char* detail) {
    GcUpdateStateLock guard;
    g_updateState.phase = phase;
    if (detail) {
        StringCchCopyA(g_updateState.detail, sizeof(g_updateState.detail), detail);
    } else {
        g_updateState.detail[0] = 0;
    }
    debug_log("update state: phase=%d detail=%s\n", (int)phase,
              detail && detail[0] ? detail : "<none>");
}

// ---------------------------------------------------------------------------
// This machine's architecture
// ---------------------------------------------------------------------------

// The RUNNING system's architecture, not the one this binary was compiled for.
// An x64 build runs perfectly well under emulation on an arm64 machine, and
// offering it the x64 setup would quietly keep that machine on the emulated
// build forever.  IsWow64Process2 reports the native machine even when the
// caller is being emulated, which is precisely the question being asked.
static GcUpdateArch service_update_host_arch() {
    typedef BOOL (WINAPI *IsWow64Process2Fn)(HANDLE, USHORT*, USHORT*);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel) {
        IsWow64Process2Fn isWow64Process2 =
            reinterpret_cast<IsWow64Process2Fn>(
                GetProcAddress(kernel, "IsWow64Process2"));
        if (isWow64Process2) {
            USHORT processMachine = 0, nativeMachine = 0;
            if (isWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
                if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) return GC_UPDATE_ARCH_ARM64;
                if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) return GC_UPDATE_ARCH_X64;
                debug_log("update: unrecognized native machine 0x%04X\n",
                          (unsigned)nativeMachine);
                return GC_UPDATE_ARCH_UNKNOWN;
            }
        }
    }
    // Pre-1709 systems have no IsWow64Process2.  Those cannot be arm64 at all,
    // so the compiled architecture is the right answer there.
#if defined(_M_ARM64) || defined(__aarch64__)
    return GC_UPDATE_ARCH_ARM64;
#else
    return GC_UPDATE_ARCH_X64;
#endif
}

// ---------------------------------------------------------------------------
// Persisted policy
// ---------------------------------------------------------------------------

static bool service_update_settings_path(char* out, size_t outSize) {
    if (out && outSize) out[0] = 0;
    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) return false;
    // Settings live in the same file as the shared profile bank, in their own
    // section, so there is one protected machine-scope document rather than two
    // with two DACLs to keep correct.
    return SUCCEEDED(StringCchCopyA(out, outSize, machinePath));
}

static void service_update_load_settings() {
    char path[MAX_PATH] = {};
    if (!service_update_settings_path(path, sizeof(path))) {
        debug_log("update settings: cannot resolve the machine config path; using defaults\n");
        return;
    }
    GcUpdateStateLock guard;
    int mode = get_config_int(path, GC_UPDATE_CONFIG_SECTION, "auto_check",
                              GC_UPDATE_AUTO_CHECK_UNSET);
    // An out-of-range value in a hand-edited file falls back to UNSET rather
    // than to ON: a file we cannot read confidently must not be what turns on
    // outbound network traffic.
    g_updateState.autoCheck = (mode == GC_UPDATE_AUTO_CHECK_ON) ? GC_UPDATE_AUTO_CHECK_ON
                            : (mode == GC_UPDATE_AUTO_CHECK_OFF) ? GC_UPDATE_AUTO_CHECK_OFF
                            : GC_UPDATE_AUTO_CHECK_UNSET;
    g_updateState.intervalSeconds = gc_update_clamp_interval(
        get_config_int(path, GC_UPDATE_CONFIG_SECTION, "interval_seconds",
                       GC_UPDATE_INTERVAL_DEFAULT_SECONDS));
    // Stored as seconds since the epoch in two halves: get_config_int is an
    // int, and a 64-bit timestamp does not fit one.  The join is pure and
    // asserted (4360-4369) because getting it wrong silently stops the updater
    // checking rather than failing anywhere visible.
    g_updateState.lastCheckUnix = gc_update_join_timestamp(
        get_config_int(path, GC_UPDATE_CONFIG_SECTION, "last_check_high", 0),
        get_config_int(path, GC_UPDATE_CONFIG_SECTION, "last_check_low", 0));
    g_updateState.consecutiveFailures = get_config_int(
        path, GC_UPDATE_CONFIG_SECTION, "consecutive_failures", 0);
    get_config_string(path, GC_UPDATE_CONFIG_SECTION, "highest_seen_version", "",
                      g_updateState.highestSeenVersion,
                      sizeof(g_updateState.highestSeenVersion));
    if (g_updateState.consecutiveFailures < 0) g_updateState.consecutiveFailures = 0;

    debug_log("update settings: autoCheck=%d interval=%ds lastCheck=%lld failures=%d\n",
              (int)g_updateState.autoCheck, g_updateState.intervalSeconds,
              g_updateState.lastCheckUnix, g_updateState.consecutiveFailures);
}

static void service_update_save_settings() {
    char path[MAX_PATH] = {};
    if (!service_update_settings_path(path, sizeof(path))) return;
    char err[256] = {};
    if (!ensure_machine_config_directory(err, sizeof(err))) {
        debug_log("update settings: cannot prepare the machine config directory: %s\n",
                  err[0] ? err : "unknown");
        return;
    }

    int mode, interval, failures;
    long long lastCheck;
    {
        GcUpdateStateLock guard;
        mode = (int)g_updateState.autoCheck;
        interval = g_updateState.intervalSeconds;
        failures = g_updateState.consecutiveFailures;
        lastCheck = g_updateState.lastCheckUnix;
    }
    set_config_int(path, GC_UPDATE_CONFIG_SECTION, "auto_check", mode);
    set_config_int(path, GC_UPDATE_CONFIG_SECTION, "interval_seconds", interval);
    set_config_int(path, GC_UPDATE_CONFIG_SECTION, "consecutive_failures", failures);
    if (g_updateState.highestSeenVersion[0]) {
        set_config_string(path, GC_UPDATE_CONFIG_SECTION, "highest_seen_version",
                          g_updateState.highestSeenVersion);
    }
    int lastCheckHigh = 0, lastCheckLow = 0;
    gc_update_split_timestamp(lastCheck, &lastCheckHigh, &lastCheckLow);
    set_config_int(path, GC_UPDATE_CONFIG_SECTION, "last_check_high", lastCheckHigh);
    set_config_int(path, GC_UPDATE_CONFIG_SECTION, "last_check_low", lastCheckLow);

    // The settings file must keep its protected DACL: it is read by the service
    // to decide whether to make outbound requests, so a standard user who could
    // rewrite it could turn the updater on for everyone.
    Win32Utf8Path widePath(path);
    if (widePath.valid_for(path)) {
        char aclErr[256] = {};
        if (!apply_protected_machine_config_dacl(widePath.value, aclErr, sizeof(aclErr))) {
            debug_log("update settings: DACL hardening failed: %s\n",
                      aclErr[0] ? aclErr : "unknown");
        }
    }
}

// ---------------------------------------------------------------------------
// Staging directory
// ---------------------------------------------------------------------------

// %ProgramData%\Green Curve\updates -- inside the already-hardened machine
// config directory, so it inherits SYSTEM+Administrators full / Users
// read-and-execute.  A standard user can see the staged installer and cannot
// replace it, which is the property the whole design turns on.
static bool service_update_staging_dir(char* out, size_t outSize, char* err, size_t errSize) {
    if (out && outSize) out[0] = 0;
    if (!ensure_machine_config_directory(err, errSize)) return false;

    char machinePath[MAX_PATH] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath))) {
        set_message(err, errSize, "Cannot resolve the machine config path");
        return false;
    }
    char* slash = strrchr(machinePath, '\\');
    if (!slash) {
        set_message(err, errSize, "Machine config path has no directory");
        return false;
    }
    *slash = 0;
    if (FAILED(StringCchPrintfA(out, outSize, "%s\\%s", machinePath,
                                GC_UPDATE_STAGING_DIR_NAME))) {
        set_message(err, errSize, "Update staging path is too long");
        return false;
    }
    if (!gc_CreateDirectoryUtf8(out, nullptr)) {
        DWORD createErr = GetLastError();
        if (createErr != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Cannot create the update staging directory (error %lu)",
                        createErr);
            out[0] = 0;
            return false;
        }
    }
    DWORD attrs = gc_GetFileAttributesUtf8(out);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        // A reparse point here would redirect an elevated write somewhere the
        // user chose.  Refuse rather than follow it.
        set_message(err, errSize, "Update staging directory is unavailable or unsafe");
        out[0] = 0;
        return false;
    }
    return true;
}

// Remove any previously staged packages.  Called before staging a new one and
// after a successful install, so a verified-but-superseded installer is never
// left lying around for a later run to pick up by name.
static void service_update_clear_staging() {
    char dir[MAX_PATH] = {};
    char err[256] = {};
    if (!service_update_staging_dir(dir, sizeof(dir), err, sizeof(err))) return;

    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, sizeof(pattern), "%s\\*", dir))) return;
    WIN32_FIND_DATAA fd = {};
    HANDLE find = gc_FindFirstFileUtf8(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    int removed = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        char filePath[MAX_PATH] = {};
        if (SUCCEEDED(StringCchPrintfA(filePath, sizeof(filePath), "%s\\%s", dir,
                                       fd.cFileName))) {
            if (gc_DeleteFileUtf8(filePath)) removed++;
        }
    } while (gc_FindNextFileUtf8(find, &fd));
    FindClose(find);

    GcUpdateStateLock guard;
    g_updateState.stagedPath[0] = 0;
    g_updateState.packageStaged = false;
    g_updateState.packageVerified = false;
    if (removed) debug_log("update staging: cleared %d stale file(s)\n", removed);
}

// ---------------------------------------------------------------------------
// Projection onto the wire
// ---------------------------------------------------------------------------

static long long service_update_now_unix() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value = {};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    // 100ns ticks since 1601 -> seconds since 1970.
    return (long long)((value.QuadPart / 10000000ULL) - 11644473600ULL);
}

// Whether this is an installed copy with an installer to upgrade, as opposed to
// a portable .7z extraction.  Read from the ARP entry the installer writes.
static bool service_update_is_installed_copy(char* installDirOut, size_t installDirSize);

// Stamped onto every response, next to outcomeSeverity, by the single place
// that writes a response out.
static void service_update_populate_response(ServiceUpdateState* out) {
    if (!out) return;
    ServiceUpdateState blank = {};
    *out = blank;
    service_update_state_init();

    // Resolved BEFORE the lock is taken.  This opens a registry key and stats a
    // directory, and it is stamped onto every single response the service
    // sends, so doing it under the lock made every response serialize against
    // the update worker for the length of two round trips to the registry and
    // the filesystem -- for a value the worker neither reads nor publishes.
    char installDir[MAX_PATH] = {};
    bool installedCopy =
        service_update_is_installed_copy(installDir, sizeof(installDir));

    GcUpdateStateLock guard;
    out->phase = (gc_u32)g_updateState.phase;
    out->decision = (gc_u32)g_updateState.decision;
    out->autoCheck = (gc_u32)g_updateState.autoCheck;
    out->intervalSeconds = (gc_u32)g_updateState.intervalSeconds;
    out->consecutiveFailures = (gc_u32)g_updateState.consecutiveFailures;
    out->lastRefusal = (gc_u32)g_updateState.lastRefusal;
    out->lastCheckUnix = (gc_u64)(g_updateState.lastCheckUnix > 0
                                      ? g_updateState.lastCheckUnix : 0);
    out->packageStaged = g_updateState.packageStaged ? 1 : 0;
    out->packageVerified = g_updateState.packageVerified ? 1 : 0;
    out->installRunning = g_updateState.installRunning ? 1 : 0;
    out->workerRunning = g_updateState.workerRunning ? 1 : 0;
    out->guiShutdownRequested = g_updateState.guiShutdownRequested ? 1 : 0;

    out->isInstalledCopy = installedCopy ? 1 : 0;

    StringCchCopyA(out->installedVersion, sizeof(out->installedVersion), APP_VERSION);
    if (g_updateState.manifestValid && g_updateState.manifest.valid) {
        StringCchCopyA(out->availableVersion, sizeof(out->availableVersion),
                       g_updateState.manifest.version.text);
        const GcUpdateAsset* asset =
            gc_update_select_asset(&g_updateState.manifest, service_update_host_arch());
        out->availableBytes = asset ? (gc_u64)asset->size : 0;
    }
    StringCchCopyA(out->detail, sizeof(out->detail), g_updateState.detail);

    // Computed here rather than in the GUI because the high-water mark is
    // machine scope and the GUI is per-user: two accounts must not disagree
    // about whether the channel went backwards.  Takes the second reserved
    // byte, like workerRunning took the first, so the struct size is unchanged
    // and an older peer reads it as the zero it always was.
    out->channelState = (gc_u8)gc_update_channel_state(
        (GcUpdateAutoCheck)g_updateState.autoCheck,
        (long long)g_updateState.lastCheckUnix, service_update_now_unix(),
        g_updateState.highestSeenVersion,
        g_updateState.manifestValid && g_updateState.manifest.valid
            ? g_updateState.manifest.version.text
            : "");
}
