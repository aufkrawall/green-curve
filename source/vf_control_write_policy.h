// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
enum VfControlWritePolicy : unsigned int {
    VF_WRITE_EXISTING_STRATEGY,
    VF_WRITE_SINGLE_BATCH,
};
static inline bool vf_control_write_policy_valid(VfControlWritePolicy policy) {
    return policy == VF_WRITE_EXISTING_STRATEGY || policy == VF_WRITE_SINGLE_BATCH;
}
