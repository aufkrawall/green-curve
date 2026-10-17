// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Protocol v29 wire record, split out of service_protocol.h (size ratchet);
// included by it right before ServiceSnapshot, which carries it last.
#ifndef GREEN_CURVE_SERVICE_AUTO_APPLY_FAILURE_H
#define GREEN_CURVE_SERVICE_AUTO_APPLY_FAILURE_H

// v29: the automatic lifecycle write (logon, standby, driver recovery) whose
// failure latched SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED in this
// service instance.  All zero when there is none: no lockout, another cause, or
// a lockout persisted by an earlier instance.  `coreRecovered` is set only when
// the apply's own core recovery verified stock clocks.
enum { SERVICE_AUTO_APPLY_FAILURE_DETAIL_CHARS = 184 };
struct ServiceAutoApplyFailure {
    gc_u32 trigger;              // ServiceLifecycleTrigger
    gc_bool8 coreRecovered;
    gc_u8 reserved[3];
    char detail[SERVICE_AUTO_APPLY_FAILURE_DETAIL_CHARS];
};
static_assert(sizeof(ServiceAutoApplyFailure) == 192,
              "ServiceAutoApplyFailure is a fixed wire record");

#endif // GREEN_CURVE_SERVICE_AUTO_APPLY_FAILURE_H
