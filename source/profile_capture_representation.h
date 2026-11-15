// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
enum ProfileCaptureRepresentation : int {
    PROFILE_CAPTURE_UNDECIDED,
    PROFILE_CAPTURE_EXACT,
    PROFILE_CAPTURE_FLATTEN,
    PROFILE_CAPTURE_PIN
};
static constexpr unsigned int profile_capture_representation_bit(ProfileCaptureRepresentation value) {
    return value >= PROFILE_CAPTURE_EXACT && value <= PROFILE_CAPTURE_PIN ? 1u << (unsigned int)value : 0u;
}
static constexpr const char *profile_capture_representation_name(ProfileCaptureRepresentation value) {
    return value == PROFILE_CAPTURE_EXACT     ? "exact"
           : value == PROFILE_CAPTURE_FLATTEN ? "flatten"
           : value == PROFILE_CAPTURE_PIN     ? "pin"
                                              : "unselected";
}
