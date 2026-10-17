// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_SCALAR_SHIFT_RUNTIME_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_SCALAR_SHIFT_RUNTIME_H

// The pre-write readback boundary for retained NVML-shift evidence. Kept out
// of the pure policy header so fixtures can execute its decisions without
// application state or GPU libraries.
static bool apply_scalar_shift_precheck_before_writes(const DesiredSettings* desired,
                                                      char* result, size_t resultSize) {
    if (!apply_scalar_shift_needs_precheck(desired, vf_curve_global_gpu_offset_supported()))
        return true;
    char scalarReadDetail[128] = {};
    nvml_read_clock_offsets(scalarReadDetail, sizeof(scalarReadDetail));
    bool offsetsOk = false;
    if (!read_live_curve_snapshot_settled(1, 0, &offsetsOk) || !offsetsOk) {
        set_message(result, resultSize,
            "Cannot read a fresh curve to validate the GPU-offset measurement. Nothing was changed.");
        debug_log("nvml shift: REFUSED before any write; fresh curve or offsets unavailable\n");
        return false;
    }
    return apply_provenance_scalar_shift_precheck(desired, g_app.readback.gpuOffset,
        g_app.gpuClockOffsetkHz, g_app.curve, result, resultSize);
}

#endif
