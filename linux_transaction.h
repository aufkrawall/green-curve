// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_TRANSACTION_H
#define GREEN_CURVE_LINUX_TRANSACTION_H

enum LinuxMutationPhase {
    LINUX_MUTATION_RESET_BASELINE = 1u << 0,
    LINUX_MUTATION_GPU_OFFSET = 1u << 1,
    LINUX_MUTATION_MEM_OFFSET = 1u << 2,
    LINUX_MUTATION_POWER = 1u << 3,
    LINUX_MUTATION_CURVE = 1u << 4,
    LINUX_MUTATION_LOCK = 1u << 5,
    LINUX_MUTATION_FAN = 1u << 6,
};

struct LinuxMutationResult {
    bool success;
    bool anyWrite;
    bool rollbackAttempted;
    bool rollbackSucceeded;
    unsigned int attemptedPhases;
    unsigned int completedPhases;
    unsigned int failedPhases;
};

typedef bool (*LinuxTransactionStepFn)(void* context, unsigned int phase);
typedef bool (*LinuxTransactionRollbackFn)(void* context, unsigned int attemptedPhases);

// Pure ordered transaction core. A failed phase may have partially written, so
// rollback receives every attempted phase, not only fully completed phases.
static inline LinuxMutationResult linux_execute_transaction(
    unsigned int requestedPhases, LinuxTransactionStepFn step,
    LinuxTransactionRollbackFn rollback, void* context) {
    LinuxMutationResult result = {};
    const unsigned int order[] = {
        LINUX_MUTATION_RESET_BASELINE, LINUX_MUTATION_GPU_OFFSET,
        LINUX_MUTATION_MEM_OFFSET, LINUX_MUTATION_POWER,
        LINUX_MUTATION_CURVE, LINUX_MUTATION_LOCK, LINUX_MUTATION_FAN,
    };
    if (!step) return result;
    for (unsigned int phase : order) {
        if (!(requestedPhases & phase)) continue;
        result.anyWrite = true;
        result.attemptedPhases |= phase;
        if (!step(context, phase)) {
            result.failedPhases |= phase;
            result.rollbackAttempted = true;
            result.rollbackSucceeded = rollback && rollback(context, result.attemptedPhases);
            return result;
        }
        result.completedPhases |= phase;
    }
    result.success = true;
    return result;
}

#endif
