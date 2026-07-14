// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_MUTATION_FORWARD_H
#define GREEN_CURVE_GUI_MUTATION_FORWARD_H

#include "gui_mutation_queue_policy.h"

#ifndef GREEN_CURVE_SERVICE_BINARY
static void auto_profile_on_mutation_completed(int slot,
    ServiceApplyOrigin origin, bool success, const char* result);
static void auto_profile_on_mutation_superseded(int slot);
static void app_launch_on_mutation_completed(int slot, bool success,
    const char* result);
static void gui_mutation_pending_was_superseded(GuiMutationUiContext context,
    int profileSlot, ServiceApplyOrigin origin, gc_u64 operationId);
static bool gui_mutation_queue_apply(const DesiredSettings* desired,
    bool interactive, ServiceApplyOrigin origin,
    ServiceProfileSource profileSource, int profileSlot,
    GuiMutationUiContext context, const char* source,
    char* status, size_t statusSize);
static void gui_mutation_advance_gpu_epoch(const char* reason);
#endif

#endif
