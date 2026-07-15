// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_SERVICE_IO_QUEUE_POLICY_H
#define GREEN_CURVE_GUI_SERVICE_IO_QUEUE_POLICY_H

enum GuiServiceReadQueueDecision {
    GUI_SERVICE_READ_QUEUE_NEW = 0,
    GUI_SERVICE_READ_COALESCE = 1,
    GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK = 2,
};

static inline GuiServiceReadQueueDecision gui_full_sync_queue_decide(
    bool fullSyncPending) {
    return fullSyncPending ? GUI_SERVICE_READ_COALESCE
                           : GUI_SERVICE_READ_QUEUE_NEW;
}

static inline GuiServiceReadQueueDecision gui_telemetry_queue_decide(
    bool fullSyncPending, bool mutationActive, bool adminActive,
    bool workerBusy, bool telemetryPending) {
    if (fullSyncPending || mutationActive || adminActive || workerBusy)
        return GUI_SERVICE_READ_DROP_BEHIND_PRIORITY_WORK;
    return telemetryPending ? GUI_SERVICE_READ_COALESCE
                            : GUI_SERVICE_READ_QUEUE_NEW;
}

static inline bool gui_state_adoption_requires_redraw_suppression(
    bool telemetry, bool authorityOrTopologyChanged,
    bool activeDesiredChanged, bool forceFullRefresh,
    bool controlsMissing) {
    return !telemetry || authorityOrTopologyChanged || activeDesiredChanged ||
        forceFullRefresh || controlsMissing;
}

static inline bool gui_state_adoption_requires_full_render(
    bool fullSync, bool authorityOrTopologyChanged,
    bool activeDesiredChanged, bool renderTopologyChanged,
    bool rebaseUnkeyedDesired, bool forceFullRefresh,
    bool controlsMissing) {
    return fullSync || authorityOrTopologyChanged || activeDesiredChanged ||
        renderTopologyChanged || rebaseUnkeyedDesired || forceFullRefresh ||
        controlsMissing;
}

#endif // GREEN_CURVE_GUI_SERVICE_IO_QUEUE_POLICY_H
