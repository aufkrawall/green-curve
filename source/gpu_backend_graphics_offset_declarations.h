// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
static bool nvapi_set_gpu_offset(int offsetkHz, bool forceWrite = false);
static bool nvapi_set_gpu_offset_precise(gc_i64 offsetHz, gc_u32 surface, bool forceWrite = false);
struct ScalarCurveSubmissionReview;
static bool nvapi_set_gpu_offset_precise_checked(gc_i64 offsetHz, gc_u32 surface, bool forceWrite, ScalarCurveSubmissionReview* review);
static bool nvapi_reset_graphics_scalar_controls(bool retireCoverage);
static bool nvapi_verify_graphics_scalar_recovery(bool retireCoverage);
#ifdef GREEN_CURVE_SERVICE_BINARY
static bool service_ownership_marker_note_graphics_surface_before_write(gc_u32);
#endif
