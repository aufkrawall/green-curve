// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Selected-display-adapter Configuration Manager notifications.  This shard is
// deliberately independent of the service control handler: it maps the
// validated NvAPI/NVML PCI identity to exactly one present display DEVINST and
// the callback only posts coalesced lifecycle evidence.  It never allocates,
// starts a thread, touches hardware, or writes a file from the CM callback.

#include <cfgmgr32.h>
#include <devguid.h>
#include "selected_gpu_pnp_policy.h"

// Implemented by the lifecycle coordinator.  These hooks may only update its
// reducer/coalesced state and signal the long-lived worker event.
static void service_lifecycle_post_selected_gpu_removal();
static void service_lifecycle_post_selected_gpu_arrival();

struct ServiceSelectedGpuDevinstMatch {
    DEVINST devInst;
    WCHAR instanceId[MAX_DEVICE_ID_LEN];
    DWORD bus;
    DWORD device;
    DWORD function;
    bool bdfValid;
};

struct ServiceSelectedGpuNotificationContext {
    volatile LONG active;
    WCHAR instanceId[MAX_DEVICE_ID_LEN];
};

static SRWLOCK g_serviceSelectedGpuNotificationLock = SRWLOCK_INIT;
static HCMNOTIFICATION g_serviceSelectedGpuNotification = nullptr;
static ServiceSelectedGpuNotificationContext g_serviceSelectedGpuNotificationContext = {};
static volatile LONG g_serviceSelectedGpuNotificationsStopping = 0;
static volatile LONGLONG g_serviceSelectedGpuEventGeneration = 0;
static volatile LONG g_serviceSelectedGpuRemoved = 0;

struct ServiceSelectedGpuWriteEpoch {
    LONGLONG generation;
    bool removed;
};

static ServiceSelectedGpuWriteEpoch service_selected_gpu_capture_write_epoch() {
    ServiceSelectedGpuWriteEpoch epoch = {};
    epoch.generation = InterlockedCompareExchange64(
        &g_serviceSelectedGpuEventGeneration, 0, 0);
    epoch.removed = InterlockedCompareExchange(
        &g_serviceSelectedGpuRemoved, 0, 0) != 0;
    return epoch;
}

static bool service_selected_gpu_write_epoch_is_current(
    const ServiceSelectedGpuWriteEpoch& epoch) {
    return !epoch.removed &&
        InterlockedCompareExchange(&g_serviceSelectedGpuRemoved, 0, 0) == 0 &&
        InterlockedCompareExchange64(
            &g_serviceSelectedGpuEventGeneration, 0, 0) == epoch.generation;
}

static bool service_selected_gpu_read_dword_property(
    HDEVINFO devices, SP_DEVINFO_DATA* device, DWORD property, DWORD* valueOut)
{
    if (!devices || devices == INVALID_HANDLE_VALUE || !device || !valueOut) return false;
    DWORD value = 0;
    DWORD type = 0;
    DWORD required = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices, device, property, &type,
            reinterpret_cast<PBYTE>(&value), sizeof(value), &required) ||
        type != REG_DWORD || required != sizeof(value) || value == 0xFFFFFFFFu) {
        return false;
    }
    *valueOut = value;
    return true;
}

static bool service_selected_gpu_read_bdf(
    HDEVINFO devices, SP_DEVINFO_DATA* device,
    DWORD* busOut, DWORD* deviceOut, DWORD* functionOut)
{
    if (!busOut || !deviceOut || !functionOut) return false;
    DWORD bus = 0;
    DWORD address = 0;
    if (!service_selected_gpu_read_dword_property(
            devices, device, SPDRP_BUSNUMBER, &bus) ||
        !service_selected_gpu_read_dword_property(
            devices, device, SPDRP_ADDRESS, &address)) return false;
    DWORD pciDevice = (address >> 16) & 0xFFFFu;
    DWORD pciFunction = address & 0xFFFFu;
    if (bus > 255u || pciDevice > 31u || pciFunction > 7u) return false;
    *busOut = bus;
    *deviceOut = pciDevice;
    *functionOut = pciFunction;
    return true;
}

static bool service_selected_gpu_devinst_matches(
    HDEVINFO devices, SP_DEVINFO_DATA* device, const GpuAdapterInfo* target,
    ServiceSelectedGpuDevinstMatch* matchOut)
{
    if (!devices || devices == INVALID_HANDLE_VALUE || !device || !target || !matchOut) {
        return false;
    }
    WCHAR hardwareIds[4096] = {};
    DWORD type = 0;
    DWORD required = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices, device, SPDRP_HARDWAREID,
            &type, reinterpret_cast<PBYTE>(hardwareIds), sizeof(hardwareIds),
            &required) || (type != REG_MULTI_SZ && type != REG_SZ)) {
        return false;
    }
    hardwareIds[ARRAY_COUNT(hardwareIds) - 1] = L'\0';
    hardwareIds[ARRAY_COUNT(hardwareIds) - 2] = L'\0';
    if (!selected_gpu_pnp_hardware_id_list_matches_target(
            target, hardwareIds, ARRAY_COUNT(hardwareIds))) return false;

    DWORD bus = 0;
    DWORD pciDevice = 0;
    DWORD function = 0;
    bool bdfValid = service_selected_gpu_read_bdf(
        devices, device, &bus, &pciDevice, &function);
    if (!selected_gpu_pnp_bdf_matches_target(
            target, bdfValid, bus, pciDevice, function)) return false;

    WCHAR instanceId[MAX_DEVICE_ID_LEN] = {};
    if (!SetupDiGetDeviceInstanceIdW(devices, device, instanceId,
            ARRAY_COUNT(instanceId), nullptr) || !instanceId[0]) return false;
    memset(matchOut, 0, sizeof(*matchOut));
    matchOut->devInst = device->DevInst;
    matchOut->bus = bus;
    matchOut->device = pciDevice;
    matchOut->function = function;
    matchOut->bdfValid = bdfValid;
    StringCchCopyW(matchOut->instanceId, ARRAY_COUNT(matchOut->instanceId), instanceId);
    return true;
}

static bool service_find_selected_gpu_devinst(
    const GpuAdapterInfo* target, ServiceSelectedGpuDevinstMatch* matchOut,
    char* err, size_t errSize)
{
    if (!target || !matchOut || !target->valid || !target->pciInfoValid ||
        !target->deviceId || !target->subSystemId ||
        target->subSystemId <= 0xFFFFu) {
        set_message(err, errSize,
            "Selected GPU has no complete PCI hardware identity");
        return false;
    }
    if (target->pciDomain != 0) {
        set_message(err, errSize,
            "Selected GPU is in unsupported PCI domain %u", target->pciDomain);
        return false;
    }

    HDEVINFO devices = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        set_message(err, errSize,
            "Cannot enumerate present display adapters (error %lu)", GetLastError());
        return false;
    }

    unsigned int matches = 0;
    ServiceSelectedGpuDevinstMatch selected = {};
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device = {};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            DWORD enumErr = GetLastError();
            if (enumErr != ERROR_NO_MORE_ITEMS) {
                debug_log("selected_gpu_pnp: display enumeration stopped at %lu (error %lu)\n",
                    index, enumErr);
            }
            break;
        }
        ServiceSelectedGpuDevinstMatch candidate = {};
        if (!service_selected_gpu_devinst_matches(
                devices, &device, target, &candidate)) continue;
        ++matches;
        if (matches == 1) selected = candidate;
    }
    SetupDiDestroyDeviceInfoList(devices);

    SelectedGpuPnpMatchResolution resolution =
        selected_gpu_pnp_resolve_match_count(matches);
    if (resolution == SELECTED_GPU_PNP_MATCH_NONE) {
        set_message(err, errSize,
            "No present display DEVINST matches the selected GPU PCI identity");
        return false;
    }
    if (resolution == SELECTED_GPU_PNP_MATCH_AMBIGUOUS) {
        set_message(err, errSize,
            "Selected GPU PCI identity is ambiguous across %u present display adapters",
            matches);
        return false;
    }
    *matchOut = selected;
    return true;
}

static DWORD CALLBACK service_selected_gpu_notification_callback(
    HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA eventData, DWORD eventDataSize)
{
    ServiceSelectedGpuNotificationContext* state =
        reinterpret_cast<ServiceSelectedGpuNotificationContext*>(context);
    if (!state || InterlockedCompareExchange(&state->active, 0, 0) == 0) {
        return ERROR_SUCCESS;
    }
    const size_t instanceIdOffset =
        offsetof(CM_NOTIFY_EVENT_DATA, u.DeviceInstance.InstanceId);
    if (!eventData || eventData->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE ||
        eventDataSize < instanceIdOffset + sizeof(WCHAR)) {
        return ERROR_SUCCESS;
    }
    size_t instanceIdChars = (eventDataSize - instanceIdOffset) / sizeof(WCHAR);
    bool terminated = false;
    for (size_t i = 0; i < instanceIdChars; ++i) {
        if (!eventData->u.DeviceInstance.InstanceId[i]) {
            terminated = true;
            break;
        }
    }
    if (!terminated || _wcsicmp(eventData->u.DeviceInstance.InstanceId,
            state->instanceId) != 0) return ERROR_SUCCESS;
    switch (action) {
        case CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED:
            InterlockedExchange(&g_serviceSelectedGpuRemoved, 1);
            InterlockedIncrement64(&g_serviceSelectedGpuEventGeneration);
            service_lifecycle_post_selected_gpu_removal();
            break;
        case CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED:
            InterlockedExchange(&g_serviceSelectedGpuRemoved, 0);
            InterlockedIncrement64(&g_serviceSelectedGpuEventGeneration);
            service_lifecycle_post_selected_gpu_arrival();
            break;
        default:
            break;
    }
    return ERROR_SUCCESS;
}

static bool service_unregister_selected_gpu_notification_locked(
    char* err, size_t errSize)
{
    InterlockedExchange(&g_serviceSelectedGpuNotificationContext.active, 0);
    if (!g_serviceSelectedGpuNotification) return true;
    CONFIGRET result = CM_Unregister_Notification(g_serviceSelectedGpuNotification);
    if (result != CR_SUCCESS) {
        set_message(err, errSize,
            "Cannot unregister selected-GPU notification (CM error 0x%08lX)",
            (unsigned long)result);
        return false;
    }
    g_serviceSelectedGpuNotification = nullptr;
    return true;
}

static bool service_unregister_selected_gpu_notification(
    char* err, size_t errSize)
{
    AcquireSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
    bool ok = service_unregister_selected_gpu_notification_locked(err, errSize);
    ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
    return ok;
}

static bool service_register_selected_gpu_notification(
    const GpuAdapterInfo* target, char* err, size_t errSize)
{
    AcquireSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
    if (InterlockedCompareExchange(
            &g_serviceSelectedGpuNotificationsStopping, 0, 0) != 0) {
        set_message(err, errSize,
            "Selected-GPU notifications are stopping");
        ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
        return false;
    }
    char unregisterErr[192] = {};
    if (!service_unregister_selected_gpu_notification_locked(
            unregisterErr, sizeof(unregisterErr))) {
        set_message(err, errSize,
            "Cannot replace selected-GPU notification: %s", unregisterErr);
        ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
        return false;
    }

    ServiceSelectedGpuDevinstMatch selected = {};
    if (!service_find_selected_gpu_devinst(target, &selected, err, errSize)) {
        ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
        return false;
    }
    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    StringCchCopyW(filter.u.DeviceInstance.InstanceId,
        ARRAY_COUNT(filter.u.DeviceInstance.InstanceId), selected.instanceId);

    StringCchCopyW(g_serviceSelectedGpuNotificationContext.instanceId,
        ARRAY_COUNT(g_serviceSelectedGpuNotificationContext.instanceId),
        selected.instanceId);
    InterlockedExchange(&g_serviceSelectedGpuNotificationContext.active, 1);
    HCMNOTIFICATION notification = nullptr;
    CONFIGRET result = CM_Register_Notification(
        &filter, &g_serviceSelectedGpuNotificationContext,
        service_selected_gpu_notification_callback, &notification);
    if (result != CR_SUCCESS || !notification) {
        InterlockedExchange(&g_serviceSelectedGpuNotificationContext.active, 0);
        set_message(err, errSize,
            "Cannot register selected-GPU notification (CM error 0x%08lX)",
            (unsigned long)result);
        ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
        return false;
    }
    g_serviceSelectedGpuNotification = notification;
    // Registration establishes a new exact target generation.  Invalidate any
    // pre-write epoch captured for the prior target and publish the newly
    // enumerated target as present.
    InterlockedExchange(&g_serviceSelectedGpuRemoved, 0);
    InterlockedIncrement64(&g_serviceSelectedGpuEventGeneration);
    if (selected.bdfValid) {
        debug_log("selected_gpu_pnp: registered exact display DEVINST at PCI %lu:%lu.%lu\n",
            selected.bus, selected.device, selected.function);
    } else {
        debug_log("selected_gpu_pnp: registered exact display DEVINST by unique full hardware ID (BDF unavailable)\n");
    }
    ReleaseSRWLockExclusive(&g_serviceSelectedGpuNotificationLock);
    return true;
}

// Exact selected-device notification is a recovery capability, not a
// prerequisite for user-authorized settings writes.  Unsupported adapters may
// lack a complete PCI identity, and Windows can transiently withhold a DEVINST
// during re-enumeration.  In either case the registration routine has already
// deactivated/unregistered the previous target, so unscoped PnP events cannot
// authorize recovery; explicit/logon/standby writes remain available.
static bool service_refresh_selected_gpu_notification_best_effort(
    const GpuAdapterInfo* target, const char* reason)
{
    char err[256] = {};
    if (service_register_selected_gpu_notification(target, err, sizeof(err))) {
        debug_log("selected_gpu_pnp: exact recovery notification ready (%s)\n",
            reason && reason[0] ? reason : "unspecified target refresh");
        return true;
    }
    debug_log("selected_gpu_pnp: automatic selected-GPU PnP recovery degraded (%s): %s; explicit/logon/standby writes remain enabled\n",
        reason && reason[0] ? reason : "unspecified target refresh",
        err[0] ? err : "unknown registration failure");
    return false;
}

static void service_prepare_selected_gpu_notification_before_running() {
    // Resolve and register the exact selected display DEVINST before RUNNING.
    // This probe is read-only: ordinary service startup never applies or resets
    // settings. Missing/unsupported PCI identity merely disables selected-PnP
    // recovery authorization until a later successful target selection.
    GpuAdapterInfo startupTarget = {};
    char startupProbeDetail[256] = {};
    lock_service_runtime();
    bool startupProbeReady = false;
    if (g_serviceControlledRecoveryValidated &&
        g_serviceControlledRecoveryTargetGpu.valid) {
        // The protected snapshot already carries the validated target. Avoid
        // touching transitional driver libraries before RUNNING.
        startupTarget = g_serviceControlledRecoveryTargetGpu;
        StringCchCopyA(startupProbeDetail,
            ARRAY_COUNT(startupProbeDetail),
            "protected controlled-recovery target (driver probe skipped)");
    } else {
        // Only enumerate NvAPI adapters/PCI metadata here. Full
        // hardware_initialize() also performs settled VF/NVML telemetry reads
        // that are unnecessary for registration and would turn startup
        // readiness into a timing-sensitive driver probe.
        startupProbeReady = nvapi_init() && nvapi_enum_gpu();
        if (!startupProbeReady) {
            StringCchCopyA(startupProbeDetail,
                ARRAY_COUNT(startupProbeDetail),
                "read-only NvAPI PCI enumeration unavailable");
        }
        if (startupProbeReady && g_app.selectedGpu.valid) {
            startupTarget = g_app.selectedGpu;
        } else if (startupProbeReady &&
            g_app.selectedGpuIndex < g_app.adapterCount) {
            startupTarget = g_app.adapters[g_app.selectedGpuIndex];
        }
    }
    unlock_service_runtime();
    debug_log("selected_gpu_pnp: startup read-only target probe ready=%d targetValid=%d detail=%s\n",
        startupProbeReady ? 1 : 0, startupTarget.valid ? 1 : 0,
        startupProbeDetail[0] ? startupProbeDetail : "none");
    service_refresh_selected_gpu_notification_best_effort(
        &startupTarget, "service startup read-only target");
}

static void service_stop_selected_gpu_notification_best_effort(
    const char* reason)
{
    InterlockedExchange(&g_serviceSelectedGpuNotificationsStopping, 1);
    char err[256] = {};
    if (!service_unregister_selected_gpu_notification(err, sizeof(err))) {
        // active was cleared before CM_Unregister_Notification was attempted,
        // so even an OS cleanup failure cannot post new recovery authority.
        debug_log("selected_gpu_pnp: notification cleanup degraded (%s): %s; callback is inactive\n",
            reason && reason[0] ? reason : "service shutdown",
            err[0] ? err : "unknown unregister failure");
    }
}
