// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Included by the GUI amalgam after its queue and transport policy types.
enum GuiServiceIoKind {
    GUI_SERVICE_IO_FULL_SYNC = 1,
    GUI_SERVICE_IO_TELEMETRY = 2,
    GUI_SERVICE_IO_ADMIN_TOGGLE = 3,
    GUI_SERVICE_IO_CAPTURE = 4,
};

struct GuiMutationWork {
    GuiMutationKind kind;
    GuiMutationUiContext context;
    ServiceRequest request;
    DesiredSettings desired;
    ServiceProfileSource profileSource;
    ServiceApplyOrigin origin;
    int profileSlot;
    LONG gpuEpoch;
    LONG presentationEpoch;
    DWORD sessionId;
    ULONGLONG queuedTickMs;
    HWND notifyWindow;
};

struct GuiMutationCompletion {
    GuiMutationWork work;
    ServiceResponse response;
    bool transportAttempted;
    bool success;
    bool serviceInstalled;
    bool serviceRunning;
    gc_u64 connectionEpoch;
    ULONGLONG durationMs;
    char result[512];
};

struct GuiServiceIoCompletion {
    GuiServiceIoKind kind;
    // The GPU a full sync asked the service to show (invalid when none), so
    // the completion can tell an honoured selection from a refused one.
    GpuAdapterInfo requestedTarget;
    gc_u64 captureToken;
    ServiceResponse response;
    bool transportSuccess;
    bool redrawControls;
    bool serviceInstalled;
    bool serviceRunning;
    gc_u64 connectionEpoch;
    LONG presentationEpoch;
    ULONGLONG durationMs;
    bool adminEnable;
    bool adminRepair;
    // GcServiceAdminReason for an admin toggle: WHY it failed, classified
    // across the elevation boundary by exit code so the GUI can say something
    // other than "exit code 1" (service_admin_reason_policy.h).
    int adminReason;
    // What the attempt proved about the service's EXISTENCE, as opposed to
    // whether this particular exchange worked. Only an unreachable pipe means
    // offline; see service_request_deadline_policy.h.
    ServiceClientSendOutcome sendOutcome;
    char reason[96];
    char error[512];
};
