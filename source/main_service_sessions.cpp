// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Immutable interactive-session profile context
// ============================================================================
//
// A logon authorization is bound to {WTS session, user SID, authentication
// LUID}.  Profile paths are resolved into this local immutable context; the
// service never uses mutable g_app.configPath to decide what an account may
// apply at logon.

struct ServiceSessionConfigContext {
    ServiceLifecycleIdentity identity;
    bool isLocalAdmin;
    unsigned int selectedGpuIndex;
    ConfiguredGpuSelection configuredGpu;
    char userConfigPath[MAX_PATH];
    char machineConfigPath[MAX_PATH];
};

static HANDLE g_servicePipeRecycleEvent = nullptr;

// Implemented by the long-lived lifecycle coordinator shard included after
// this one.  The SCM control handler and authenticated pipe handoff only post
// compact events; they never allocate a worker or touch hardware.
static bool service_lifecycle_post_session_event(DWORD eventType, DWORD sessionId);
static bool service_lifecycle_post_logon_handoff(
    const ServiceLifecycleIdentity* identity);
static void service_lifecycle_post_prerequisite_signal(const char* reason);
static bool service_has_pending_logon_apply();
static void service_lifecycle_worker_reduce_logoff(
    const ServiceLifecycleIdentity* identity, DWORD sessionId);
static void service_lifecycle_worker_note_identity_not_ready(
    DWORD sessionId, ServiceLifecycleTrigger trigger);
static void service_lifecycle_worker_queue_logon(
    const ServiceLifecycleIdentity* identity,
    ServiceLifecycleTrigger trigger,
    const char* reason);

static bool service_resolve_session_config_context(
    DWORD sessionId,
    ServiceSessionConfigContext* context,
    char* err,
    size_t errSize)
{
    if (!context) return false;
    memset(context, 0, sizeof(*context));

    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token)) {
        set_message(err, errSize, "WTSQueryUserToken failed (error %lu)", GetLastError());
        return false;
    }

    bool ok = service_identity_from_token(token, sessionId,
        &context->identity, err, errSize);
    if (!ok) {
        CloseHandle(token);
        return false;
    }
    context->isLocalAdmin = token_is_local_admin(token);

    char localAppData[MAX_PATH] = {};
    PWSTR localAppDataW = nullptr;
    HRESULT folderResult = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, token, &localAppDataW);
    if (SUCCEEDED(folderResult) && localAppDataW) {
        copy_wide_to_utf8(localAppDataW, localAppData, ARRAY_COUNT(localAppData));
    }
    if (localAppDataW) CoTaskMemFree(localAppDataW);

    // A profile directory fallback is useful on systems where Known Folders
    // cannot yet resolve during the first moments of logon.  It is still
    // derived from this exact token and remains local to the context.
    if (!localAppData[0]) {
        WCHAR profileDirW[MAX_PATH] = {};
        DWORD profileSize = ARRAY_COUNT(profileDirW);
        char profileDir[MAX_PATH] = {};
        if (!GetUserProfileDirectoryW(token, profileDirW, &profileSize) ||
            !copy_wide_to_utf8(profileDirW, profileDir, ARRAY_COUNT(profileDir)) ||
            FAILED(StringCchPrintfA(localAppData, ARRAY_COUNT(localAppData),
                "%s\\AppData\\Local", profileDir))) {
            CloseHandle(token);
            set_message(err, errSize, "The logging-on user's profile path is not ready");
            return false;
        }
    }
    CloseHandle(token);

    if (FAILED(StringCchPrintfA(context->userConfigPath,
            ARRAY_COUNT(context->userConfigPath), "%s\\Green Curve\\%s",
            localAppData, CONFIG_FILE_NAME))) {
        set_message(err, errSize, "The logging-on user's config path is too long");
        memset(context, 0, sizeof(*context));
        return false;
    }
    resolve_machine_config_path(context->machineConfigPath,
        ARRAY_COUNT(context->machineConfigPath));
    // selected_index is intentionally loaded later, under the same watched,
    // cross-process transaction as the logon selector and profile contents.
    // Resolving it here used to permit a stale GPU + fresh profile combination.
    return true;
}

enum ServiceSessionIdentityCheckResult {
    SERVICE_SESSION_IDENTITY_MATCH = 0,
    SERVICE_SESSION_IDENTITY_TRANSIENT,
    SERVICE_SESSION_IDENTITY_SUPERSEDED,
};

static ServiceSessionIdentityCheckResult service_verify_active_session_identity(
    const ServiceLifecycleIdentity* expected,
    ServiceLifecycleIdentity* currentOut,
    char* detail,
    size_t detailSize)
{
    if (currentOut) memset(currentOut, 0, sizeof(*currentOut));
    if (!expected || !expected->valid) {
        set_message(detail, detailSize, "missing expected session identity");
        return SERVICE_SESSION_IDENTITY_SUPERSEDED;
    }

    DWORD activeSessionId = (DWORD)-1;
    if (!get_active_interactive_session_id(&activeSessionId)) {
        set_message(detail, detailSize, "no active interactive session");
        return SERVICE_SESSION_IDENTITY_TRANSIENT;
    }
    if (activeSessionId != expected->sessionId) {
        // A task triggered at logon can run just before Windows marks its WTS
        // session active, and fast-user switching can temporarily make another
        // session active. Neither event supersedes this authenticated login.
        // Retain it until a real readiness signal, matching logoff, or a token
        // with a different authentication LUID proves that the incarnation was
        // replaced.
        ServiceLifecycleIdentity expectedSessionNow = {};
        char identityDetail[128] = {};
        if (service_resolve_session_identity(expected->sessionId,
                &expectedSessionNow, identityDetail,
                sizeof(identityDetail)) &&
            !service_lifecycle_identity_equal(expected,
                &expectedSessionNow)) {
            set_message(detail, detailSize,
                "session %lu authentication identity was superseded while inactive",
                (unsigned long)expected->sessionId);
            return SERVICE_SESSION_IDENTITY_SUPERSEDED;
        }
        set_message(detail, detailSize,
            "session %lu is not active yet (active session %lu)",
            (unsigned long)expected->sessionId,
            (unsigned long)activeSessionId);
        return SERVICE_SESSION_IDENTITY_TRANSIENT;
    }

    ServiceLifecycleIdentity current = {};
    if (!service_resolve_session_identity(activeSessionId, &current,
            detail, detailSize)) {
        return SERVICE_SESSION_IDENTITY_TRANSIENT;
    }
    if (!service_lifecycle_identity_equal(expected, &current)) {
        set_message(detail, detailSize,
            "session identity changed (session=%lu expectedAuth=%llu currentAuth=%llu)",
            (unsigned long)activeSessionId,
            (unsigned long long)expected->authenticationId,
            (unsigned long long)current.authenticationId);
        return SERVICE_SESSION_IDENTITY_SUPERSEDED;
    }
    if (currentOut) *currentOut = current;
    return SERVICE_SESSION_IDENTITY_MATCH;
}

static bool service_enter_config_storage_lock_interruptible(
    HANDLE* acquiredMutex) {
    return enter_config_storage_lock_interruptible(
        g_serviceStopEvent, acquiredMutex);
}

static ServiceLogonProfileResolveResult service_load_logon_profile_from_context(
    ServiceSessionConfigContext* context,
    DesiredSettings* desired,
    int* slotOut,
    ServiceProfileSource* sourceOut)
{
    if (!context || !context->identity.valid || !desired || !slotOut || !sourceOut) {
        return SERVICE_LOGON_PROFILE_INVALID;
    }
    memset(desired, 0, sizeof(*desired));
    *slotOut = 0;
    *sourceOut = SERVICE_PROFILE_SOURCE_NONE;

    // Read the selector, referenced slots, machine default and policy as one
    // cross-process config transaction.  Every Green Curve writer holds this
    // mutex across its atomic rewrite; taking it once here prevents a logon
    // attempt from combining the old shared selector with the new per-user
    // selector (or vice versa) and incorrectly concluding that no profile was
    // configured.  The nested config helpers are intentionally recursive on
    // both the process critical section and the Win32 mutex.
    HANDLE configMutex = nullptr;
    if (!service_enter_config_storage_lock_interruptible(&configMutex)) {
        debug_log("logon profile context: config transaction lock unavailable; keeping intent pending\n");
        return SERVICE_LOGON_PROFILE_TRANSIENT;
    }
    ServiceLogonProfileResolveResult transactionResult = [&]() {

    DWORD attrs = GetFileAttributesA(context->userConfigPath);
    bool userConfigPresent = attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;

    int userSlot = get_config_int(context->userConfigPath,
        "profiles", "logon_slot", 0);
    if (userSlot < 0 || userSlot > CONFIG_NUM_SLOTS) userSlot = 0;
    bool hasPerUser = userSlot > 0 &&
        is_profile_slot_saved(context->userConfigPath, userSlot);
    bool perUserPending = userSlot > 0 && !hasPerUser;

    int sharedSlot = get_config_int(context->userConfigPath,
        "profiles", "logon_shared_slot", 0);
    if (sharedSlot < 0 || sharedSlot > CONFIG_NUM_SLOTS) sharedSlot = 0;
    bool haveMachinePath = context->machineConfigPath[0] != 0;
    bool hasShared = haveMachinePath && sharedSlot > 0 &&
        is_machine_profile_slot_saved(sharedSlot);
    bool sharedPending = sharedSlot > 0 && !hasShared;

    int machineSlot = 0;
    bool hasMachineDefault = get_machine_logon_slot(&machineSlot) &&
        machineSlot > 0 && haveMachinePath &&
        is_machine_profile_slot_saved(machineSlot);
    bool machineDefaultPending = machineSlot > 0 && !hasMachineDefault;

    bool policyActive = false;
    if (!get_machine_restrict_policy(&policyActive)) {
        debug_log("logon profile context: protected shared-only policy is unreadable; failing closed until config readiness changes\n");
        return SERVICE_LOGON_PROFILE_TRANSIENT;
    }
    LogonProfileSource selected = resolve_logon_profile_source(
        policyActive, context->isLocalAdmin, sharedSlot, hasShared, userSlot,
        hasPerUser, hasMachineDefault);
    debug_log("logon profile context: session=%lu sid=%s auth=%llu config=%s present=%d policy=%d admin=%d shared=%d/%d user=%d/%d machine=%d/%d source=%d\n",
        (unsigned long)context->identity.sessionId,
        context->identity.sid,
        (unsigned long long)context->identity.authenticationId,
        context->userConfigPath,
        userConfigPresent ? 1 : 0, policyActive ? 1 : 0,
        context->isLocalAdmin ? 1 : 0,
        sharedSlot, hasShared ? 1 : 0,
        userSlot, hasPerUser ? 1 : 0,
        machineSlot, hasMachineDefault ? 1 : 0,
        (int)selected);

    char loadErr[256] = {};
    auto load_and_validate = [&](const char* path, int slot) -> bool {
        return path && path[0] &&
            load_profile_from_config(path, slot, desired, loadErr, sizeof(loadErr)) &&
            desired_settings_have_explicit_state(desired, true,
                loadErr, sizeof(loadErr));
    };
    auto bind_user_gpu = [&]() -> bool {
        char gpuErr[256] = {};
        if (!userConfigPresent ||
            !load_configured_gpu_selection(context->userConfigPath,
                &context->configuredGpu, gpuErr, sizeof(gpuErr))) {
            debug_log("logon profile context: per-user GPU binding is not coherent for session=%lu: %s\n",
                (unsigned long)context->identity.sessionId,
                gpuErr[0] ? gpuErr : "user config is not mounted");
            return false;
        }
        context->selectedGpuIndex = context->configuredGpu.legacyIndex;
        return true;
    };
    auto bind_machine_gpu = [&](int slot) -> bool {
        char gpuErr[256] = {};
        if (load_machine_profile_gpu_selection(slot,
                &context->configuredGpu, gpuErr, sizeof(gpuErr))) {
            context->selectedGpuIndex = context->configuredGpu.legacyIndex;
            debug_log("logon profile context: shared slot %d uses published GPU binding legacy=%u stable=%d\n",
                slot, context->configuredGpu.legacyIndex,
                context->configuredGpu.stableIdentityPresent ? 1 : 0);
            return true;
        }
        // Compatibility for profiles published by older versions. The later
        // immutable target resolver accepts this weak ordinal only when the
        // machine has exactly one adapter; multi-GPU systems fail closed.
        memset(&context->configuredGpu, 0, sizeof(context->configuredGpu));
        context->selectedGpuIndex = 0;
        debug_log("logon profile context: shared slot %d has no coherent GPU binding (%s); allowing legacy ordinal 0 only if hardware proves single-adapter\n",
            slot, gpuErr[0] ? gpuErr : "missing section");
        return true;
    };

    switch (selected) {
        case LOGON_PROFILE_SOURCE_PENDING:
            debug_log("logon profile context: explicit selection is not coherently materialized; keeping intent pending\n");
            return SERVICE_LOGON_PROFILE_TRANSIENT;

        case LOGON_PROFILE_SOURCE_SHARED_BANK:
            if (load_and_validate(context->machineConfigPath, sharedSlot) &&
                bind_machine_gpu(sharedSlot)) {
                *slotOut = sharedSlot;
                *sourceOut = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
                return SERVICE_LOGON_PROFILE_RESOLVED;
            }
            break;

        case LOGON_PROFILE_SOURCE_PER_USER:
            if (load_and_validate(context->userConfigPath, userSlot) &&
                bind_user_gpu()) {
                *slotOut = userSlot;
                *sourceOut = SERVICE_PROFILE_SOURCE_USER_SLOT;
                return SERVICE_LOGON_PROFILE_RESOLVED;
            }
            break;

        case LOGON_PROFILE_SOURCE_MACHINE_DEFAULT:
            if (load_and_validate(context->machineConfigPath, machineSlot) &&
                bind_machine_gpu(machineSlot)) {
                *slotOut = machineSlot;
                *sourceOut = SERVICE_PROFILE_SOURCE_MACHINE_SLOT;
                return SERVICE_LOGON_PROFILE_RESOLVED;
            }
            break;

        case LOGON_PROFILE_SOURCE_NONE:
        default:
            // A missing account config at early logon normally means the user
            // profile has not mounted yet.  It is never proof that the user has
            // no configured profile, so retain the authorization until a real
            // config/identity signal or logoff.  Likewise, a selector that is
            // present while its referenced slot is temporarily unreadable is a
            // prerequisite failure, not a terminal no-profile result.
            bool eligiblePerUserPending = perUserPending &&
                (!policyActive || context->isLocalAdmin);
            if (!userConfigPresent || eligiblePerUserPending || sharedPending ||
                machineDefaultPending) {
                return SERVICE_LOGON_PROFILE_TRANSIENT;
            }
            return SERVICE_LOGON_PROFILE_NONE;
    }

    debug_log("logon profile context: configured profile failed validation: %s\n",
        loadErr[0] ? loadErr : "unknown");
    // A selected profile can be temporarily unreadable while another process
    // atomically replaces the INI or while the user profile finishes mounting.
    // Treat this as a config prerequisite, never as permission to fall back to
    // an arbitrary profile or as a terminal hardware failure. The lifecycle
    // worker arms directory notifications and retries only on a real change.
    return SERVICE_LOGON_PROFILE_TRANSIENT;
    }();
    leave_config_storage_lock(configMutex);
    return transactionResult;
}

static bool service_logoff_identity_from_cached_state(
    DWORD sessionId,
    ServiceLifecycleIdentity* identityOut);

// Called only by the lifecycle worker.  Only WTS_SESSION_LOGON creates a WTS
// logon authorization.  Connect/disconnect/unlock events are readiness cues;
// they must never synthesize a new login for whichever session happens to be
// active when the worker runs.
static void service_handle_session_change(DWORD eventType, DWORD eventSessionId) {
    const char* name = "other";
    switch (eventType) {
        case WTS_SESSION_LOGON: name = "WTS logon"; break;
        case WTS_SESSION_LOGOFF: name = "WTS logoff"; break;
        case WTS_CONSOLE_CONNECT: name = "console connect"; break;
        case WTS_CONSOLE_DISCONNECT: name = "console disconnect"; break;
        case WTS_SESSION_UNLOCK: name = "session unlock"; break;
        default: break;
    }
    debug_log("lifecycle session cue: %s session=%lu\n", name,
        (unsigned long)eventSessionId);

    if (eventType == WTS_SESSION_UNLOCK ||
        eventType == WTS_CONSOLE_CONNECT ||
        eventType == WTS_CONSOLE_DISCONNECT) {
        service_lifecycle_post_prerequisite_signal(name);
        if (g_servicePipeRecycleEvent) SetEvent(g_servicePipeRecycleEvent);
        return;
    }

    if (eventType == WTS_SESSION_LOGOFF) {
        ServiceLifecycleIdentity loggedOff = {};
        // Prefer the cached incarnation.  By the time the worker processes a
        // logoff, Windows may already have reused the numeric session and
        // WTSQueryUserToken would then describe the *new* authentication LUID.
        if (!service_logoff_identity_from_cached_state(
                eventSessionId, &loggedOff)) {
            service_resolve_session_identity(eventSessionId, &loggedOff,
                nullptr, 0);
        }
        service_lifecycle_worker_reduce_logoff(&loggedOff, eventSessionId);
        if (g_servicePipeRecycleEvent) SetEvent(g_servicePipeRecycleEvent);
        return;
    }

    if (eventType != WTS_SESSION_LOGON) return;

    DWORD activeSessionId = (DWORD)-1;
    if (!get_active_interactive_session_id(&activeSessionId) ||
        activeSessionId != eventSessionId) {
        // Bind the pending prerequisite to the session that actually received
        // the WTS logon.  A later task handoff/connect cue may resolve it once
        // that exact session becomes active; never apply the old active user's
        // profile because Windows delivered the notification slightly early.
        service_lifecycle_worker_note_identity_not_ready(eventSessionId,
            SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON);
        debug_log("lifecycle session cue: WTS logon session %lu is not active yet (active=%lu); exact identity remains pending\n",
            (unsigned long)eventSessionId, (unsigned long)activeSessionId);
        return;
    }
    ServiceLifecycleIdentity active = {};
    char identityErr[256] = {};
    if (!service_resolve_session_identity(eventSessionId, &active,
            identityErr, sizeof(identityErr))) {
        debug_log("lifecycle session cue: active identity not ready: %s\n",
            identityErr[0] ? identityErr : "unknown");
        service_lifecycle_worker_note_identity_not_ready(eventSessionId,
            SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON);
        return;
    }
    service_lifecycle_worker_queue_logon(&active,
        SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON, name);
    if (g_servicePipeRecycleEvent) SetEvent(g_servicePipeRecycleEvent);
}
