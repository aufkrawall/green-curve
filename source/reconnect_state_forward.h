// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_RECONNECT_STATE_FORWARD_H
#define GREEN_CURVE_RECONNECT_STATE_FORWARD_H

// Protocol-v11 authority stamp. A new service process gets a random identity;
// the GPU generation advances whenever selected-device authority is lost.
static gc_u64 g_serviceInstanceId = 0;
static volatile LONGLONG g_serviceStateRevision = 0;
static volatile LONGLONG g_serviceGpuGeneration = 1;
static volatile LONG g_serviceGpuPhase = SERVICE_GPU_PHASE_STARTING;

static bool service_initialize_state_identity();
static void service_publish_gpu_phase(ServiceGpuPhase, bool, const char*);
static void populate_service_state_response(ServiceResponse*);
static bool service_mutation_preconditions_match(
    const ServiceRequest*, char*, size_t);

#ifndef GREEN_CURVE_SERVICE_BINARY
static void gui_draft_begin_user_edit();
static void gui_draft_capture_curve_value(int, const char*);
static void gui_draft_capture_desired(const DesiredSettings*);
static void gui_draft_capture_text(char*, size_t, const char*);
static void gui_service_begin_full_sync(const char*);
static void gui_service_retry_full_sync(const char*);
static void gui_selected_gpu_notification_refresh(const GpuAdapterInfo*);
static void gui_selected_gpu_notification_unregister();
#endif

static void reset_gui_gdi_generation(const char*);

#endif // GREEN_CURVE_RECONNECT_STATE_FORWARD_H
