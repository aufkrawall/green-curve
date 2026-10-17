// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The apply's verified-recovery sentence, shared by its one producer
// (gpu_backend_apply_failure.h) and the service, which matches it in the full
// apply result to tell the user the clocks are back at stock
// (auto_apply_failure_notice_policy.h).  Dependency-free on purpose: the
// clock transition fixture includes the producer without the app headers.
#ifndef GREEN_CURVE_APPLY_RECOVERY_TEXT_H
#define GREEN_CURVE_APPLY_RECOVERY_TEXT_H

#define GC_APPLY_RECOVERY_VERIFIED_TEXT "Recovery verified core defaults"

#endif // GREEN_CURVE_APPLY_RECOVERY_TEXT_H
