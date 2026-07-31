// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure, injectable activation sequence used by Linux service installation.

#ifndef GREEN_CURVE_LINUX_SERVICE_INSTALL_POLICY_H
#define GREEN_CURVE_LINUX_SERVICE_INSTALL_POLICY_H

#include <stddef.h>

enum LinuxServiceActivationStep {
    LINUX_SERVICE_STEP_DAEMON_RELOAD = 0,
    LINUX_SERVICE_STEP_ENABLE,
    // The resume unit is enabled as its own step so a failure names itself.
    // Folding it into the step above would report a missing standby restore as
    // "systemctl enable failed" and leave the reader guessing which unit.
    LINUX_SERVICE_STEP_ENABLE_RESUME,
    LINUX_SERVICE_STEP_RESTART,
    LINUX_SERVICE_STEP_IS_ACTIVE,
    LINUX_SERVICE_STEP_VERIFY_SOCKET,
    LINUX_SERVICE_STEP_VERIFY_PROTOCOL,
    LINUX_SERVICE_STEP_COUNT,
};

typedef bool (*LinuxServiceActivationRunner)(
    void* context, LinuxServiceActivationStep step,
    char* error, size_t errorSize);

struct LinuxServiceActivationResult {
    bool success;
    LinuxServiceActivationStep failedStep;
    unsigned int completedSteps;
};

static inline const char* linux_service_activation_step_name(
    LinuxServiceActivationStep step) {
    switch (step) {
        case LINUX_SERVICE_STEP_DAEMON_RELOAD: return "systemctl daemon-reload";
        case LINUX_SERVICE_STEP_ENABLE: return "systemctl enable";
        case LINUX_SERVICE_STEP_ENABLE_RESUME:
            return "systemctl enable greencurve-resume.service";
        case LINUX_SERVICE_STEP_RESTART: return "systemctl restart";
        case LINUX_SERVICE_STEP_IS_ACTIVE: return "systemctl is-active";
        case LINUX_SERVICE_STEP_VERIFY_SOCKET:
            return "daemon socket pathname authorization verification";
        case LINUX_SERVICE_STEP_VERIFY_PROTOCOL: return "daemon protocol/build verification";
        default: return "unknown service activation step";
    }
}

static inline LinuxServiceActivationResult linux_service_run_activation(
    LinuxServiceActivationRunner runner, void* context,
    char* error, size_t errorSize) {
    LinuxServiceActivationResult result = {};
    result.failedStep = LINUX_SERVICE_STEP_DAEMON_RELOAD;
    if (error && errorSize) error[0] = 0;
    if (!runner) {
        if (error && errorSize) {
            const char* message = "service activation runner is missing";
            size_t i = 0;
            for (; i + 1 < errorSize && message[i]; ++i) error[i] = message[i];
            error[i] = 0;
        }
        return result;
    }
    for (unsigned int value = 0; value < LINUX_SERVICE_STEP_COUNT; ++value) {
        LinuxServiceActivationStep step =
            (LinuxServiceActivationStep)value;
        result.failedStep = step;
        if (!runner(context, step, error, errorSize)) return result;
        ++result.completedSteps;
    }
    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// What to tell the user about greencurve group membership after a successful
// install.
//
// This used to be an unconditional "add your user: sudo usermod -aG ...", which
// is wrong in the two cases that matter most.  greencurve-setup.sh runs
// --service-install and *then* enrolls the account, so the instruction printed
// immediately before the script carried it out; and a re-install by an already
// enrolled user was told to fix a non-problem.  linux_daemon_transport.cpp
// already established the rule for the client-side diagnostic -- "advertising
// usermod on a run that works would send someone chasing a non-problem" -- and
// this is the same rule applied to the installer.
//
// Pure so the decision is testable without touching /etc/group.
// ---------------------------------------------------------------------------
enum LinuxGroupEnrollmentAdvice {
    // The account is already a member: confirm access, prescribe nothing.
    LINUX_GROUP_ADVICE_ALREADY_ENROLLED = 0,
    // A known account still needs enrolling: name it, so the command can be
    // pasted as-is by whoever ran sudo.
    LINUX_GROUP_ADVICE_ENROLL_NAMED,
    // No account could be resolved (a plain root console, no SUDO_USER, no
    // login session): fall back to the generic "$USER" form.
    LINUX_GROUP_ADVICE_ENROLL_UNKNOWN,
};

static inline LinuxGroupEnrollmentAdvice linux_group_enrollment_advice(
    const char* account, bool groupExists, bool accountInGroup) {
    // No group means the install did not get far enough for membership to be
    // meaningful; generic advice is the only honest answer.
    if (!groupExists) return LINUX_GROUP_ADVICE_ENROLL_UNKNOWN;
    if (!account || !account[0]) return LINUX_GROUP_ADVICE_ENROLL_UNKNOWN;
    // Membership is only believable for an account we actually resolved, so the
    // flag is deliberately ignored without one.
    if (accountInGroup) return LINUX_GROUP_ADVICE_ALREADY_ENROLLED;
    return LINUX_GROUP_ADVICE_ENROLL_NAMED;
}

// Formats the group paragraph of the install summary.  Always NUL-terminates
// within outSize and never writes past it.
static inline void linux_format_group_enrollment_advice(
    LinuxGroupEnrollmentAdvice advice, const char* account,
    char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    switch (advice) {
        case LINUX_GROUP_ADVICE_ALREADY_ENROLLED:
            // No command: saying "then sign out/in" here would imply the user
            // still has something to do.
            gc_snprintf(out, outSize,
                        "Account '%s' is already in the greencurve group, so it "
                        "can control the GPU without sudo.\n",
                        account ? account : "");
            return;
        case LINUX_GROUP_ADVICE_ENROLL_NAMED:
            gc_snprintf(out, outSize,
                        "To control the GPU without sudo, add the account:\n"
                        "  sudo usermod -aG greencurve %s\n"
                        "Then sign out/in, or run: newgrp greencurve\n",
                        account ? account : "");
            return;
        case LINUX_GROUP_ADVICE_ENROLL_UNKNOWN:
        default:
            gc_snprintf(out, outSize,
                        "To control the GPU without sudo, add your user:\n"
                        "  sudo usermod -aG greencurve \"$USER\"\n"
                        "Then sign out/in, or run: newgrp greencurve\n");
            return;
    }
}

#endif // GREEN_CURVE_LINUX_SERVICE_INSTALL_POLICY_H
