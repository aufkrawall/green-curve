// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Service request validation, target binding, and explicit-supersession policy.

struct ServicePolicyConfigLockGuard {
    HANDLE mutex;
    bool held;
    ServicePolicyConfigLockGuard() : mutex(nullptr), held(false) {
        held = enter_config_storage_lock(&mutex);
    }
    ~ServicePolicyConfigLockGuard() {
        if (held) leave_config_storage_lock(mutex);
    }
    bool locked() const { return held; }
    ServicePolicyConfigLockGuard(const ServicePolicyConfigLockGuard&) = delete;
    ServicePolicyConfigLockGuard& operator=(
        const ServicePolicyConfigLockGuard&) = delete;
};

static bool service_path_has_reparse_component(const char* absPath, char* err, size_t errSize) {
    if (!absPath || !absPath[0]) return false;
    char probe[MAX_PATH] = {};
    if (FAILED(StringCchCopyA(probe, ARRAY_COUNT(probe), absPath))) {
        set_message(err, errSize, "Path is too long");
        return true;
    }

    size_t len = strlen(probe);
    size_t rootLen = 0;
    if (len >= 3 && probe[1] == ':' && (probe[2] == '\\' || probe[2] == '/')) {
        rootLen = 3;
    } else if (len >= 2 && probe[0] == '\\' && probe[1] == '\\') {
        const char* serverEnd = strpbrk(probe + 2, "\\/");
        if (serverEnd) {
            const char* shareEnd = strpbrk(serverEnd + 1, "\\/");
            if (shareEnd) rootLen = (size_t)(shareEnd - probe + 1);
        }
    }

    if (rootLen == 0 || rootLen >= len) return false;
    for (size_t i = rootLen; i < len; i++) {
        if (probe[i] != '\\' && probe[i] != '/') continue;
        char saved = probe[i];
        probe[i] = 0;
        DWORD attrs = gc_GetFileAttributesUtf8(probe);
        probe[i] = saved;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }

    char* slash = strrchr(probe, '\\');
    char* slashAlt = strrchr(probe, '/');
    if (!slash || (slashAlt && slashAlt > slash)) slash = slashAlt;
    if (slash && (size_t)(slash - probe) >= rootLen) {
        *slash = 0;
        DWORD attrs = gc_GetFileAttributesUtf8(probe);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }
    return false;
}

static bool normalize_windows_compare_path(const WCHAR* input, WCHAR* output,
    size_t outputCount) {
    if (!input || !output || outputCount == 0) return false;
    output[0] = 0;
    if (wcsncmp(input, L"\\\\?\\UNC\\", 8) == 0) {
        return SUCCEEDED(StringCchCopyW(output, outputCount, L"\\\\")) &&
            SUCCEEDED(StringCchCatW(output, outputCount, input + 8));
    }
    const WCHAR* start = wcsncmp(input, L"\\\\?\\", 4) == 0
        ? input + 4 : input;
    return SUCCEEDED(StringCchCopyW(output, outputCount, start));
}

// Containment against an arbitrary root.
//
// Both service write scopes need identical canonicalization and identical
// boundary handling -- equal, or followed by a separator, so `%USERPROFILE%2`
// is not treated as inside `%USERPROFILE%`.  Only the root and the refusal
// wording differ, so factoring this out is what lets the machine-config scope
// exist without a second, subtly different comparison.
static bool service_path_is_within_directory(const char* candidateUtf8,
    const char* rootUtf8, const char* outsideMessage, char* err, size_t errSize) {
    if (!candidateUtf8 || !candidateUtf8[0] || !rootUtf8 || !rootUtf8[0]) {
        set_message(err, errSize, "Containment root is unavailable");
        return false;
    }
    GcWideUtf8Arg candidate(candidateUtf8);
    GcWideUtf8Arg profile(rootUtf8);
    if (!candidate.valid_for(candidateUtf8) ||
        !profile.valid_for(rootUtf8)) {
        set_message(err, errSize, "Path contains invalid UTF-8");
        return false;
    }
    WCHAR candidateFull[MAX_PATH] = {};
    WCHAR profileFull[MAX_PATH] = {};
    DWORD candidateLength = GetFullPathNameW(candidate.value,
        ARRAY_COUNT(candidateFull), candidateFull, nullptr);
    DWORD profileLength = GetFullPathNameW(profile.value,
        ARRAY_COUNT(profileFull), profileFull, nullptr);
    if (candidateLength == 0 || candidateLength >= ARRAY_COUNT(candidateFull) ||
        profileLength == 0 || profileLength >= ARRAY_COUNT(profileFull)) {
        set_message(err, errSize, "Path cannot be canonicalized");
        return false;
    }
    WCHAR candidateNormalized[MAX_PATH] = {};
    WCHAR profileNormalized[MAX_PATH] = {};
    if (!normalize_windows_compare_path(candidateFull, candidateNormalized,
            ARRAY_COUNT(candidateNormalized)) ||
        !normalize_windows_compare_path(profileFull, profileNormalized,
            ARRAY_COUNT(profileNormalized))) {
        set_message(err, errSize, "Canonical path is too long");
        return false;
    }
    size_t profileChars = wcslen(profileNormalized);
    size_t candidateChars = wcslen(candidateNormalized);
    if (candidateChars < profileChars ||
        CompareStringOrdinal(candidateNormalized, (int)profileChars,
            profileNormalized, (int)profileChars, TRUE) != CSTR_EQUAL ||
        (candidateChars > profileChars &&
         candidateNormalized[profileChars] != L'\\')) {
        set_message(err, errSize, "%s", outsideMessage ? outsideMessage
                                                       : "Path is outside its allowed directory");
        return false;
    }
    return true;
}

static bool service_path_is_within_resolved_profile(const char* candidateUtf8,
    char* err, size_t errSize) {
    if (!g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User profile path is unavailable");
        return false;
    }
    return service_path_is_within_directory(candidateUtf8, g_serviceUserProfileDir,
        "Path is outside the caller's profile directory", err, errSize);
}

// The service's own machine-scope root.  Resolved on every call rather than
// cached: it is two registry-free string operations, and a cached root is one
// more piece of state that can be stale when the service is the thing writing.
static bool service_path_is_within_machine_config(const char* candidateUtf8,
    char* err, size_t errSize) {
    char root[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(root, sizeof(root))) {
        set_message(err, errSize, "Machine configuration directory is unavailable");
        return false;
    }
    return service_path_is_within_directory(candidateUtf8, root,
        "Path is outside the machine configuration directory", err, errSize);
}

static bool service_validate_file_write_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    if (strstr(path, "..")) {
        set_message(err, errSize, "Path contains parent directory references");
        return false;
    }
    if (strchr(path, '*') || strchr(path, '?')) {
        set_message(err, errSize, "Path contains wildcard characters");
        return false;
    }
    int colonCount = 0;
    for (const char* p = path; *p; ++p) {
        if (*p == ':') colonCount++;
    }
    if (colonCount > 1) {
        set_message(err, errSize, "Path contains invalid colon characters");
        return false;
    }
    char absPath[MAX_PATH] = {};
    DWORD len = gc_GetFullPathNameUtf8(path, ARRAY_COUNT(absPath), absPath, nullptr);
    if (len == 0 || len >= ARRAY_COUNT(absPath)) {
        set_message(err, errSize, "Invalid path");
        return false;
    }
    if (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    if (!service_path_is_within_resolved_profile(absPath, err, errSize))
        return false;
    if (service_path_has_reparse_component(absPath, err, errSize)) {
        return false;
    }
    return true;
}

// Re-open the file that was just written and check where the handle actually
// landed, so a junction planted between the write and the check cannot move the
// result out of its scope.  The scope selects the containment root; everything
// else is identical, because the redirect risk is the same either way.
static bool service_verify_written_file_path_scoped(const char* path,
    GcServiceWriteScope scope, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    // Only the caller-profile scope depends on a resolved user; the machine
    // config directory exists whether or not anyone is logged on, and requiring
    // a user here is what made the cache unwritable during early service start.
    if (scope == GC_SERVICE_WRITE_CALLER_PROFILE &&
        (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0])) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    HANDLE h = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // If we cannot open the file for verification, fail closed.
        set_message(err, errSize, "Cannot verify written file path");
        return false;
    }
    char finalPath[MAX_PATH] = {};
    DWORD len = gc_GetFinalPathNameByHandleUtf8(h, finalPath, ARRAY_COUNT(finalPath), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (len == 0 || len >= ARRAY_COUNT(finalPath)) {
        set_message(err, errSize, "Cannot resolve written file path");
        return false;
    }
    return scope == GC_SERVICE_WRITE_MACHINE_CONFIG
               ? service_path_is_within_machine_config(finalPath, err, errSize)
               : service_path_is_within_resolved_profile(finalPath, err, errSize);
}

static bool service_verify_written_file_path(const char* path, char* err, size_t errSize) {
    return service_verify_written_file_path_scoped(
        path, GC_SERVICE_WRITE_CALLER_PROFILE, err, errSize);
}

static bool service_resolve_configured_gpu_target(
    const ConfiguredGpuSelection* configured, GpuAdapterInfo* targetOut,
    char* err, size_t errSize) {
    if (targetOut) memset(targetOut, 0, sizeof(*targetOut));
    if (!configured || !targetOut) {
        set_message(err, errSize, "Published GPU binding is missing");
        return false;
    }
    char initDetail[256] = {};
    if (!hardware_initialize(initDetail, sizeof(initDetail))) {
        set_message(err, errSize, "%s", initDetail[0] ? initDetail :
            "Hardware initialization failed while resolving the published GPU");
        return false;
    }
    unsigned int resolved = configured->legacyIndex;
    ConfiguredGpuResolveResult resolution = resolve_configured_gpu_selection(
        configured, g_app.adapters, g_app.adapterCount, &resolved);
    if (resolution == CONFIGURED_GPU_RESOLVE_LEGACY_ORDINAL &&
        g_app.adapterCount != 1) {
        set_message(err, errSize,
            "This legacy shared profile has no safe GPU binding on a multi-adapter system; ask an administrator to republish it");
        return false;
    }
    if (resolution == CONFIGURED_GPU_RESOLVE_NOT_FOUND ||
        resolution == CONFIGURED_GPU_RESOLVE_AMBIGUOUS ||
        resolved >= g_app.adapterCount || resolved >= MAX_GPU_ADAPTERS ||
        !g_app.adapters[resolved].valid) {
        set_message(err, errSize,
            "The GPU bound to this shared profile is missing or ambiguous; ask an administrator to republish it");
        return false;
    }
    const GpuAdapterInfo& target = g_app.adapters[resolved];
    if (!target.pciInfoValid && g_app.adapterCount > 1) {
        set_message(err, errSize,
            "The shared profile GPU identity is ambiguous on this system");
        return false;
    }
    if (target.pciInfoValid && !gpu_adapter_has_valid_pci_location(&target) &&
        !gpu_adapter_pci_base_identity_is_unique(
            &target, g_app.adapters, g_app.adapterCount)) {
        set_message(err, errSize,
            "The shared profile GPU identity is duplicated and has no PCI location");
        return false;
    }
    *targetOut = target;
    debug_log("service shared GPU binding: resolved legacy=%u result=%d to nvapi=%u stable=%d name=%s\n",
        configured->legacyIndex, (int)resolution, target.nvapiIndex,
        configured->stableIdentityPresent ? 1 : 0,
        target.name[0] ? target.name : "<unnamed>");
    return true;
}

static bool service_apply_shared_only_policy(ServiceRequest* request,
    bool callerIsAdmin, const char* callerUser,
    bool* enforcePublishedGpuBinding,
    ConfiguredGpuSelection* publishedGpuBinding,
    char* err, size_t errSize) {
    if (enforcePublishedGpuBinding) *enforcePublishedGpuBinding = false;
    if (publishedGpuBinding) {
        memset(publishedGpuBinding, 0, sizeof(*publishedGpuBinding));
    }
    if (!request || !enforcePublishedGpuBinding || !publishedGpuBinding) {
        set_message(err, errSize, "Invalid shared-profile policy request");
        return false;
    }
    if (callerIsAdmin) return true;
    ServicePolicyConfigLockGuard policyLock;
    if (!policyLock.locked()) {
        set_message(err, errSize,
            "The machine profile policy is temporarily unavailable; no settings were applied.");
        debug_log("service APPLY rejected: shared-only policy transaction lock unavailable for non-admin caller %s\n",
            callerUser && callerUser[0] ? callerUser : "<unknown>");
        return false;
    }
    bool restrictShared = false;
    bool policyReadable = get_machine_restrict_policy(&restrictShared);
    if (!policyReadable) {
        debug_log("service APPLY rejected: protected shared-only policy is unreadable for non-admin caller %s\n",
            callerUser && callerUser[0] ? callerUser : "<unknown>");
        set_message(err, errSize,
            "The machine profile policy is temporarily unavailable; no settings were applied.");
        return false;
    }
    if (!restrictShared) return true;
    if (!(request->flags & SERVICE_REQUEST_FLAG_SHARED_SLOT)) {
        debug_log("service APPLY rejected: shared-only policy active; caller %s is not a machine admin and did not request a shared slot\n",
            callerUser && callerUser[0] ? callerUser : "<unknown>");
        set_message(err, errSize,
            "Your administrator restricts this PC to shared profiles. Use \"Shared profiles...\" to load and apply one.");
        return false;
    }

    int sharedSlot = (int)((request->flags >>
        SERVICE_REQUEST_SHARED_SLOT_SHIFT) & SERVICE_REQUEST_SHARED_SLOT_MASK);
    char machinePath[MAX_PATH] = {};
    DesiredSettings sharedDesired = {};
    char loadErr[256] = {};
    if (sharedSlot < 1 || sharedSlot > CONFIG_NUM_SLOTS ||
        !resolve_machine_config_path(machinePath, sizeof(machinePath)) ||
        !is_machine_profile_slot_saved(sharedSlot) ||
        !load_profile_from_config(machinePath, sharedSlot, &sharedDesired,
            loadErr, sizeof(loadErr))) {
        debug_log("service APPLY rejected: shared slot %d unavailable for restricted caller %s: %s\n",
            sharedSlot, callerUser && callerUser[0] ? callerUser : "<unknown>",
            loadErr[0] ? loadErr : "unknown");
        set_message(err, errSize,
            "That shared profile is no longer available. Ask your administrator.");
        return false;
    }
    char gpuSection[32] = {};
    StringCchPrintfA(gpuSection, ARRAY_COUNT(gpuSection),
        "profile%d_gpu", sharedSlot);
    bool gpuBindingPresent = config_section_has_keys(machinePath, gpuSection);
    if (gpuBindingPresent &&
        !load_machine_profile_gpu_selection(sharedSlot,
            publishedGpuBinding, loadErr, sizeof(loadErr))) {
        debug_log("service APPLY rejected: shared slot %d has malformed published GPU binding for restricted caller %s: %s\n",
            sharedSlot, callerUser && callerUser[0] ? callerUser : "<unknown>",
            loadErr[0] ? loadErr : "unknown");
        set_message(err, errSize,
            "That shared profile has an invalid GPU binding. Ask your administrator to republish it.");
        return false;
    }
    if (!gpuBindingPresent) {
        debug_log("service APPLY: shared slot %d has legacy missing GPU binding; single-adapter compatibility check required\n",
            sharedSlot);
    }

    sharedDesired.resetOcBeforeApply = true;
    request->desired = sharedDesired;
    request->resetOcBeforeApply = 1u;
    request->profileSource = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
    request->profileSlot = (gc_u32)sharedSlot;
    *enforcePublishedGpuBinding = true;
    debug_log("service APPLY: restricted caller %s applying admin shared slot %d (authoritative settings and GPU binding)\n",
        callerUser && callerUser[0] ? callerUser : "<unknown>", sharedSlot);
    return true;
}

static bool service_prepare_requested_gpu(const ServiceRequest* request, char* err, size_t errSize) {
    if (!request) {
        set_message(err, errSize, "Missing service request");
        return false;
    }
    if (!request->targetGpu.valid) {
        char currentDetail[256] = {};
        if (!hardware_initialize(currentDetail, sizeof(currentDetail))) {
            set_message(err, errSize, "%s",
                currentDetail[0] ? currentDetail :
                "Hardware initialization failed");
            return false;
        }
        return true;
    }
    if (request->targetGpu.nvapiIndex >= MAX_GPU_ADAPTERS) {
        set_message(err, errSize, "Requested GPU index is invalid");
        return false;
    }

    char initDetail[256] = {};
    if (!hardware_initialize(initDetail, sizeof(initDetail))) {
        set_message(err, errSize, "%s", initDetail[0] ? initDetail : "Hardware initialization failed");
        return false;
    }
    if (request->targetGpu.nvapiIndex >= g_app.adapterCount) {
        set_message(err, errSize, "Requested GPU is no longer available");
        debug_log("service gpu target: rejected nvapi=%u outside live adapter count %u\n",
            request->targetGpu.nvapiIndex, g_app.adapterCount);
        return false;
    }

    GpuAdapterInfo live = g_app.adapters[request->targetGpu.nvapiIndex];
    if (!live.valid) {
        set_message(err, errSize, "Requested GPU is not valid");
        return false;
    }
    bool haveStrongIdentity = request->targetGpu.pciInfoValid && live.pciInfoValid;
    if (haveStrongIdentity && !gpu_adapter_has_same_pci_identity(&live, &request->targetGpu)) {
        set_message(err, errSize, "Requested GPU identity no longer matches");
        debug_log("service gpu target: rejected nvapi=%u because PCI identity changed\n",
            request->targetGpu.nvapiIndex);
        return false;
    }
    bool bothHaveBdf = gpu_adapter_has_valid_pci_location(&live) &&
        gpu_adapter_has_valid_pci_location(&request->targetGpu);
    if (haveStrongIdentity && !bothHaveBdf &&
        !gpu_adapter_pci_base_identity_is_unique(
            &request->targetGpu, g_app.adapters, g_app.adapterCount)) {
        set_message(err, errSize,
            "Requested GPU PCI identity is ambiguous without matching BDF data");
        debug_log("service gpu target: rejected nvapi=%u because base PCI identity is duplicated and BDF is incomplete\n",
            request->targetGpu.nvapiIndex);
        return false;
    }
    if (!haveStrongIdentity && g_app.adapterCount > 1) {
        set_message(err, errSize, "Requested GPU identity is ambiguous");
        debug_log("service gpu target: rejected nvapi=%u without PCI identity on multi-adapter system\n",
            request->targetGpu.nvapiIndex);
        return false;
    }

    bool change = !g_app.selectedGpuIdentityValid ||
        g_app.selectedGpuIndex != live.nvapiIndex ||
        (haveStrongIdentity && !gpu_adapter_has_same_pci_identity(&g_app.selectedGpu, &live));
    if (change) {
        debug_log("service gpu target: selecting validated nvapi=%u nvml=%u identity=%s name=%s\n",
            live.nvapiIndex,
            live.nvmlIndex,
            haveStrongIdentity ? "pci" : "single-adapter-ordinal",
            live.name[0] ? live.name : "<unnamed>");
        reset_gpu_runtime_selection();
        g_app.selectedGpuIndex = live.nvapiIndex;
        g_app.selectedNvmlIndex = live.nvmlIndex;
        g_app.selectedGpuExplicit = true;
        g_app.selectedGpu = live;
        g_app.selectedGpuIdentityValid = live.valid;
        g_app.selectedGpuOrdinalFallback = !haveStrongIdentity;
        // Complete the read-only target initialization now so callers can
        // bind the exact DEVINST before their sole hardware write. The apply
        // routine's own initialize call then returns from its ready fast path.
        initDetail[0] = 0;
        if (!hardware_initialize(initDetail, sizeof(initDetail))) {
            set_message(err, errSize, "%s",
                initDetail[0] ? initDetail :
                "Selected GPU initialization failed");
            return false;
        }
        if (haveStrongIdentity &&
            (!g_app.selectedGpu.pciInfoValid ||
             !gpu_adapter_has_same_pci_identity(
                 &g_app.selectedGpu, &request->targetGpu))) {
            set_message(err, errSize,
                "Selected GPU identity changed during initialization");
            return false;
        }
    }
    return true;
}

static bool service_request_apply_origin_valid(const ServiceRequest* request) {
    if (!request) return false;
    ServiceApplyOrigin origin = (ServiceApplyOrigin)request->applyOrigin;
    return service_apply_origin_is_client_apply(origin);
}

// A client may only be credited with running profile N when profile N's stored
// record really describes the intent under discussion.  `candidateIntent` is the
// raw APPLY payload before the write, and the intent the service actually ended
// up holding after it; both are answered by the same predicate so the two
// checks cannot drift apart.
//
// The record is read with PROFILE_READ_FOR_OWNERSHIP.  The editor projection
// re-derives lockTracksAnchor and drops points from the LIVE VF curve, so the
// same file decodes differently as the GPU boosts -- exactly the defect
// applied_profile_indicator_policy.h documents on the GUI side.  Both sides of
// an ownership comparison are records, and records do not drift.
static bool service_profile_record_describes_intent(
    ServiceProfileSource source, int slot,
    const DesiredSettings* candidateIntent,
    char* detail, size_t detailSize, bool allowUnclaimedFan = false)
{
    if (!candidateIntent) {
        set_message(detail, detailSize, "no intent to compare");
        return false;
    }
    if (!service_profile_metadata_claims_slot((unsigned int)source, slot,
            CONFIG_NUM_SLOTS)) {
        set_message(detail, detailSize, "not a named profile slot");
        return false;
    }
    char profilePath[MAX_PATH] = {};
    if (source == SERVICE_PROFILE_SOURCE_USER_SLOT) {
        if (!g_app.configPath[0]) {
            set_message(detail, detailSize, "no per-user config path is resolved");
            return false;
        }
        StringCchCopyA(profilePath, ARRAY_COUNT(profilePath), g_app.configPath);
    } else if (!resolve_machine_config_path(profilePath,
                   ARRAY_COUNT(profilePath))) {
        set_message(detail, detailSize, "machine profile bank is unavailable");
        return false;
    }
    DesiredSettings profile = {};
    if (!load_profile_from_config(profilePath, slot, &profile,
            detail, detailSize, PROFILE_READ_FOR_OWNERSHIP)) {
        return false;
    }
    return desired_settings_match_active_service_intent(&profile,
        candidateIntent, detail, detailSize, allowUnclaimedFan);
}

// Pre-write check.  A complete profile record arrives verbatim from the tray,
// hotkey, and app-start paths, which load the slot and send it whole; proving
// the claim here is what lets the caller replace active ownership outright.
// Returns the outcome so the caller does not have to re-derive that permission
// from the source/slot it was handed.
static ServiceProfileIdentityOutcome
service_validate_requested_profile_metadata(
    ServiceRequest* request,
    ServiceProfileSource* sourceOut,
    unsigned int* slotOut)
{
    if (sourceOut) *sourceOut = SERVICE_PROFILE_SOURCE_AD_HOC;
    if (slotOut) *slotOut = 0;
    if (!request) return SERVICE_PROFILE_IDENTITY_AD_HOC;

    ServiceProfileSource source = (ServiceProfileSource)request->profileSource;
    int slot = (int)request->profileSlot;
    if (!service_profile_metadata_claims_slot((unsigned int)source, slot,
            CONFIG_NUM_SLOTS)) {
        return SERVICE_PROFILE_IDENTITY_AD_HOC;
    }

    char detail[256] = {};
    bool payloadMatches = service_profile_record_describes_intent(source, slot,
        &request->desired, detail, sizeof(detail));
    ServiceProfileIdentityOutcome outcome = service_profile_identity_outcome(
        (unsigned int)source, slot, CONFIG_NUM_SLOTS, payloadMatches, false);
    if (outcome != SERVICE_PROFILE_IDENTITY_FROM_REQUEST) {
        // NOT the end of the story: a GUI Apply is deliberately a delta and is
        // re-checked against the resulting intent once the write succeeds.
        debug_log("service APPLY: request payload does not itself prove profile metadata source=%u slot=%d: %s (the resulting intent is re-checked after the write)\n",
            (unsigned int)source, slot,
            detail[0] ? detail : "profile does not match request");
        return outcome;
    }
    debug_log("service APPLY: profile metadata proven by %s source=%u slot=%d (%s)\n",
        service_profile_identity_outcome_name(outcome),
        (unsigned int)source, slot, detail[0] ? detail : "match");
    if (sourceOut) *sourceOut = source;
    if (slotOut) *slotOut = (unsigned int)slot;
    return outcome;
}

// Post-write check.  The GUI's Apply omits every domain the editor is not
// changing -- most often the fan, because re-writing an unchanged fan policy
// would disturb a running curve mid-game (capture_gui_apply_settings()).  Such
// a payload can never equal a complete profile record field-for-field, so the
// pre-write check above refuses it and the slot the user had just applied was
// recorded as ad-hoc: the tray menu lost its checkmark and the tooltip said
// "Manual settings" until the same profile was re-picked from the tray.
//
// The honest question is not "is the payload the profile" but "is the profile
// what is now in force".  After a successful write the service holds exactly
// that -- the delta merged over the intent it already owned -- so ask it there.
// This is the same predicate the GUI's applied-profile indicator evaluates, so
// the recorded identity and the displayed tick can no longer disagree.
//
// Called with the service runtime lock held, which is what serializes
// g_serviceActiveDesired against every other writer.
static void service_confirm_profile_metadata_from_active_intent(
    const ServiceRequest* request,
    ServiceProfileSource* sourceInOut,
    unsigned int* slotInOut)
{
    if (!request || !sourceInOut || !slotInOut) return;
    // Already proven by the payload itself; nothing to upgrade.
    if (*sourceInOut != SERVICE_PROFILE_SOURCE_AD_HOC) return;

    ServiceProfileSource claimed = (ServiceProfileSource)request->profileSource;
    int slot = (int)request->profileSlot;
    if (!service_profile_metadata_claims_slot((unsigned int)claimed, slot,
            CONFIG_NUM_SLOTS)) {
        return;
    }
    if (!g_serviceHasActiveDesired) {
        debug_log("service APPLY: cannot confirm claimed profile source=%u slot=%d without an active intent; ownership stays ad-hoc\n",
            (unsigned int)claimed, slot);
        return;
    }
    DesiredSettings activeIntent = g_serviceActiveDesired;
    char detail[256] = {};
    bool activeIntentMatches = service_profile_record_describes_intent(claimed,
        slot, &activeIntent, detail, sizeof(detail), true);
    ServiceProfileIdentityOutcome outcome = service_profile_identity_outcome(
        (unsigned int)claimed, slot, CONFIG_NUM_SLOTS, false,
        activeIntentMatches);
    if (outcome != SERVICE_PROFILE_IDENTITY_FROM_ACTIVE_INTENT) {
        debug_log("service APPLY: resulting intent does not match claimed profile source=%u slot=%d: %s; ownership stays ad-hoc\n",
            (unsigned int)claimed, slot,
            detail[0] ? detail : "profile does not match resulting intent");
        return;
    }
    *sourceInOut = claimed;
    *slotInOut = (unsigned int)slot;
    debug_log("service APPLY: profile metadata proven by %s source=%u slot=%d (%s)\n",
        service_profile_identity_outcome_name(outcome),
        (unsigned int)claimed, slot, detail[0] ? detail : "match");
}

// Publish the profile identity of a successful APPLY.  The pre-write proof is
// whatever the request handler already established; the post-write one is
// resolved here, so the pipe handler never has to know there are two.  Called
// with the service runtime lock held.
static void service_record_apply_profile_identity(const ServiceRequest* request,
    ServiceProfileSource source, unsigned int slot)
{
    service_confirm_profile_metadata_from_active_intent(request, &source, &slot);
    EnterCriticalSection(&g_appLock);
    g_serviceActiveProfileSource = source;
    g_serviceActiveProfileSlot = slot;
    LeaveCriticalSection(&g_appLock);
}

// The runtime lock serializes this with lifecycle writes and helper
// preparation. Return true when the GPU was already known to be transitional;
// the explicit request still supersedes old automatic work, but must not touch
// the unstable driver in that case.
static bool service_explicit_supersede_automatic_work_locked(
    DWORD callerSessionId, const char* reason) {
    bool unsafeDriverTransition = nvml_crash_recovery_active();
    EnterCriticalSection(&g_appLock);
    unsafeDriverTransition = unsafeDriverTransition || g_app.deviceRemoved;
    LeaveCriticalSection(&g_appLock);

    InterlockedIncrement(&g_serviceExplicitSupersessionEpoch);
    if (InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) != 0 ||
        InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_serviceRestartHelperProcess,
            nullptr, nullptr) != nullptr) {
        service_abort_controlled_restart(reason);
    }
    service_discard_validated_controlled_recovery_locked(reason);
    service_lifecycle_cancel_automatic_work(reason);
    service_lifecycle_note_explicit_session_supersession(
        callerSessionId, reason);

    // Cancel process-local recovery cues after capturing whether the driver is
    // unsafe. A queued pre-supersession restart observes the epoch change; a
    // later monitor pass has no stale cue from the abandoned generation.
    EnterCriticalSection(&g_appLock);
    g_app.deviceRemoved = false;
    g_app.pendingDeviceRecovery = false;
    LeaveCriticalSection(&g_appLock);
    g_nvmlCrashCount = 0;
    g_nvmlCrashTickMs = 0;
    InterlockedExchange(&g_serviceReapplyInProgress, 0);
    service_abandon_current_recovery_evidence();
    return unsafeDriverTransition;
}
