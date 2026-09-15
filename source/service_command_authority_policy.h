// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which caller may issue which service command (F-03-002).
//
// THE DEFECT THIS EXISTS FOR (audit 2026-09-15, 0.25.2):
//
//   main_service_pipe.cpp carried ONE authorization tier, spelled as an inline
//   list inside a single `if`: nine commands required medium integrity plus the
//   active interactive session, and everything else required neither.  That is
//   the right rule for APPLY and RESET, which are per-SESSION hardware intent
//   owned by the person at the console.  SERVICE_CMD_SET_UPDATE_POLICY was in
//   the same list -- and it is not session-scoped at all.  It writes a
//   PERSISTENT, MACHINE-WIDE setting: whether the LocalSystem service checks
//   for updates, and how often.  A standard (non-administrator) user at the
//   console could therefore turn off automatic update checking for every
//   account on the machine, permanently and across reboots, with no elevation,
//   no UAC prompt and no administrative override.  Denial of patching for a
//   SYSTEM service, by an unprivileged principal.
//
// THE RULE: a command's authorization tier follows the SCOPE of what it
// mutates, not how dangerous it looks.
//
//   READ          - answers from published state; the pipe ACL is the gate.
//   CONTROL       - per-session hardware/diagnostic intent: medium integrity
//                   and the active interactive session.
//   MACHINE_ADMIN - persistent machine-wide configuration: CONTROL plus
//                   membership of the local Administrators group.
//
// A TABLE rather than an inline condition because the defect was a mapping
// error, not a logic error: the `if` had no place to say what tier a command
// belongs to, so a machine-scope command could join a session-scope list
// without anything looking wrong.  Being pure, the mapping is asserted for
// EVERY command on both hosts, so a new command cannot silently default into
// the weakest tier -- service_command_authority_tier() has no `default:`
// branch that guesses.
//
// WHY GROUP MEMBERSHIP AND NOT ELEVATION.  MACHINE_ADMIN is deliberately
// "is a member of Administrators" (token_is_local_admin(), the same predicate
// service_apply_shared_only_policy() already trusts for the restricted-profile
// decision), NOT "is running elevated".  The update-policy checkbox lives in
// the ordinary, non-elevated GUI, so requiring elevation would break a working
// feature for the very administrators who are supposed to own the setting,
// while requiring membership refuses exactly the principal the finding names:
// a standard user.  On a non-elevated admin token the Administrators SID is
// present as deny-only, so this admits non-elevated admins by design.

#ifndef GREEN_CURVE_SERVICE_COMMAND_AUTHORITY_POLICY_H
#define GREEN_CURVE_SERVICE_COMMAND_AUTHORITY_POLICY_H

#include "service_protocol.h"

enum ServiceCommandAuthorityTier {
    SERVICE_COMMAND_TIER_READ = 0,
    SERVICE_COMMAND_TIER_CONTROL,
    SERVICE_COMMAND_TIER_MACHINE_ADMIN,
};

// The tier every command belongs to.  Exhaustive on purpose: an unknown command
// number is treated as MACHINE_ADMIN so a protocol addition that forgets this
// table fails closed rather than open.  (validate_service_request_for_ipc()
// already refuses unknown command numbers outright; this is the second answer.)
static inline ServiceCommandAuthorityTier service_command_authority_tier(
    unsigned int command) {
    switch (command) {
        // Reads. These answer from state the service already publishes and
        // perform no mutation; the pipe ACL plus the state-envelope gate own
        // the decision. Unchanged from the pre-F-03-002 behaviour.
        case SERVICE_CMD_PING:
        case SERVICE_CMD_GET_SNAPSHOT:
        case SERVICE_CMD_GET_TELEMETRY:
        case SERVICE_CMD_GET_ACTIVE_DESIRED:
        case SERVICE_CMD_GET_OPERATION_RESULT:
        case SERVICE_CMD_GET_STARTUP_POLICY:
        case SERVICE_CMD_GET_UPDATE_STATE:
            return SERVICE_COMMAND_TIER_READ;

        // Per-session control: hardware intent, diagnostics written on the
        // caller's behalf, the logon handoff, and the two one-shot update
        // actions. CHECK_FOR_UPDATE and INSTALL_UPDATE stay here deliberately:
        // both are one-shot, neither persists a setting, and INSTALL_UPDATE
        // ends in a signature-verified artifact whose `userConsented` property
        // depends on it being reachable from a client request.
        case SERVICE_CMD_APPLY:
        case SERVICE_CMD_RESET:
        case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
        case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
        case SERVICE_CMD_WRITE_PROBE_REPORT:
        case SERVICE_CMD_LOGON_HANDOFF:
        case SERVICE_CMD_CHECK_FOR_UPDATE:
        case SERVICE_CMD_INSTALL_UPDATE:
        case SERVICE_CMD_RESUME_RESTORE:
        case SERVICE_CMD_REFRESH_STARTUP_PROFILE:
            return SERVICE_COMMAND_TIER_CONTROL;

        // Persistent machine-wide configuration.
        //
        // SET_UPDATE_POLICY is the finding: it decides whether this machine
        // ever fetches a security fix again, for every account on it.
        //
        // SET_STARTUP_POLICY is classified here for the same structural reason
        // -- it persists what the service applies at BOOT, before any user logs
        // on, so it is machine state by definition. Note this changes NOTHING
        // today: the command is Linux-only (linux_daemon.cpp), the Windows pipe
        // switch has no case for it and answers "Unsupported service command"
        // either way, and the Linux daemon authorizes by socket group rather
        // than through this table. It is classified now so that a future
        // Windows implementation inherits the correct tier instead of the
        // weakest one.
        case SERVICE_CMD_SET_UPDATE_POLICY:
        case SERVICE_CMD_SET_STARTUP_POLICY:
            return SERVICE_COMMAND_TIER_MACHINE_ADMIN;

        default:
            return SERVICE_COMMAND_TIER_MACHINE_ADMIN;
    }
}

static inline bool service_command_requires_medium_integrity(
    unsigned int command) {
    return service_command_authority_tier(command) !=
        SERVICE_COMMAND_TIER_READ;
}

static inline bool service_command_requires_local_admin(unsigned int command) {
    return service_command_authority_tier(command) ==
        SERVICE_COMMAND_TIER_MACHINE_ADMIN;
}

// The whole decision in one place, so the log line and the accept/refuse answer
// cannot drift apart -- the same reason service_request_reject_reason() exists.
// Returns nullptr when the caller may issue the command, or the refusal text.
static inline const char* service_command_authority_reject_reason(
    unsigned int command, unsigned int callerIntegrityRid,
    unsigned int mediumIntegrityRid, bool callerIsLocalAdmin) {
    if (service_command_requires_medium_integrity(command) &&
        callerIntegrityRid < mediumIntegrityRid) {
        return "Service control requires a medium-integrity client";
    }
    if (service_command_requires_local_admin(command) && !callerIsLocalAdmin) {
        return "Changing machine-wide Green Curve policy requires an administrator";
    }
    return nullptr;
}

#endif // GREEN_CURVE_SERVICE_COMMAND_AUTHORITY_POLICY_H
