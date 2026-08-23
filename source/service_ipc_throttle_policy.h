// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure admission-control policy for the Windows service pipe transport.
//
// The 2026-08-22 incident showed the single serialized pipe-server path can be
// monopolized by authenticated local processes that connect and then stall
// before (or while) sending their request. The transport now probes the fixed
// 12-byte request header first, impersonates the verified client only after
// that mandatory first read (pre-read impersonation fails with
// ERROR_CANNOT_IMPERSONATE -- see llm-wiki/log/recent.md), derives a stable
// logon identity, and asks THIS module whether to continue.
//
// Everything here is pure and host-portable: callers inject a monotonic
// millisecond clock (GetTickCount64 in production) and observe only decisions.
// No allocation, no OS types, no raw identity logging (callers log reason codes
// only). See temp/transition-safe.md for the full design.

#ifndef GREEN_CURVE_SERVICE_IPC_THROTTLE_POLICY_H
#define GREEN_CURVE_SERVICE_IPC_THROTTLE_POLICY_H

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tunables. One named-constant block, covered by the regression boundaries.
// These are conservative starting points, not product promises.
// ---------------------------------------------------------------------------
enum {
    // Fixed-size identity table (open addressing). Bounded memory: nothing is
    // ever sized from wire values.
    SERVICE_IPC_TABLE_SLOTS = 128,

    // Normal-class bucket: burst 80, refill 20 tokens/second. Ordinary GUI
    // polls cost 1 token, so a healthy client can never feel this.
    SERVICE_IPC_BUCKET_BURST_MILLI = 80 * 1000,
    SERVICE_IPC_REFILL_PER_SECOND_MILLI = 20 * 1000,

    // Reserved logon-handoff bucket, charged independently of the normal
    // bucket so GUI traffic cannot starve the scheduled transition. A real
    // handoff happens once per login; the slow refill bounds abuse.
    SERVICE_IPC_HANDOFF_RESERVE_MILLI = 8 * 1000,
    SERVICE_IPC_HANDOFF_REFILL_PER_SECOND_MILLI = 1 * 1000,

    // Costs. Transport faults weigh far more than completed requests so a
    // stalled/truncating client burns its own budget quickly while ordinary
    // polling stays cheap.
    SERVICE_IPC_COST_SUCCESS = 1,
    SERVICE_IPC_COST_BAD_COMMAND = 5,
    SERVICE_IPC_COST_TRANSPORT_FAULT = 10,

    // Idle expiry: an unused identity leaves the table completely; there is
    // no permanent ban and no persisted ban state anywhere.
    SERVICE_IPC_IDLE_EXPIRY_MS = 60 * 1000,

    // Small global budget charged ONLY when impersonation fails after the
    // mandatory first read (or another unidentifiable fault occurs). Honest
    // clients never hit this path; it must never poison a real user's
    // per-identity quota.
    SERVICE_IPC_ANON_BUDGET_MAX = 32,
    SERVICE_IPC_ANON_REFILL_PER_SECOND_MILLI = 1 * 1000,

    // Fixed SID string capacity; must equal ServiceLifecycleIdentity::sid
    // (asserted in the regression harness so the copies cannot drift).
    SERVICE_IPC_KEY_SID_BYTES = 184,
};

// Wrap-safe monotonic comparison for GetTickCount64()-style counters.
// Equivalent to (a >= b) without undefined behaviour at the wrap point.
inline bool service_ipc_time_at_or_after(unsigned long long a,
                                         unsigned long long b) {
    return (long long)(a - b) >= 0;
}

// ---------------------------------------------------------------------------
// Throttle identity: stable logon identity, deliberately NOT a bare PID or
// bare session number. user SID + authentication LUID + session id. A fresh
// login (new authentication LUID/session) starts with fresh quotas, so a new
// session never inherits the previous session's penalties.
// ---------------------------------------------------------------------------
struct ServiceIpcThrottleKey {
    enum { kSidBytes = SERVICE_IPC_KEY_SID_BYTES };
    bool valid;
    unsigned int sessionId;
    unsigned long long authenticationId;
    char sid[kSidBytes];

    void clear() {
        valid = false;
        sessionId = 0;
        authenticationId = 0;
        memset(sid, 0, sizeof(sid));
    }

    void fill(unsigned int session, unsigned long long authId,
              const char* sidText) {
        clear();
        if (!sidText || !sidText[0]) return;
        size_t i = 0;
        for (; i + 1 < kSidBytes && sidText[i]; ++i) sid[i] = sidText[i];
        sid[i] = 0;
        sessionId = session;
        authenticationId = authId;
        valid = true;
    }

    bool equals(const ServiceIpcThrottleKey& other) const {
        return valid && other.valid && sessionId == other.sessionId &&
               authenticationId == other.authenticationId &&
               strcmp(sid, other.sid) == 0;
    }
};

// FNV-1a over the packed identity fields. Deterministic, allocation-free.
inline unsigned int service_ipc_key_hash(const ServiceIpcThrottleKey& key) {
    if (!key.valid) return 0u;
    unsigned int hash = 2166136261u;
    for (size_t i = 0; i < ServiceIpcThrottleKey::kSidBytes; ++i) {
        hash ^= (unsigned char)key.sid[i];
        hash *= 16777619u;
    }
    for (int i = 0; i < 4; ++i) {
        hash ^= (unsigned char)((key.sessionId >> (8 * i)) & 0xFFu);
        hash *= 16777619u;
    }
    for (int i = 0; i < 8; ++i) {
        hash ^= (unsigned char)((key.authenticationId >> (8 * i)) & 0xFFu);
        hash *= 16777619u;
    }
    hash ^= 0xFFu; // fold the terminating NUL into the mix
    hash *= 16777619u;
    return hash ? hash : 1u;
}

// ---------------------------------------------------------------------------
// Command classification. Classification drives only admission priority; it
// is never authorization. A hostile client can lie here, so every lane keeps
// its own strict budget and the post-body validator stays authoritative.
// ---------------------------------------------------------------------------
enum ServiceIpcRequestClass {
    SERVICE_IPC_CLASS_NORMAL = 0,
    SERVICE_IPC_CLASS_HANDOFF = 1,
    SERVICE_IPC_CLASS_LIFECYCLE = 2,
    SERVICE_IPC_CLASS_BULK_OUTPUT = 3,
    SERVICE_IPC_CLASS_UNKNOWN = 4,
};

inline ServiceIpcRequestClass service_ipc_classify_command(
        unsigned int command) {
    // Values mirror enum ServiceCommand (service_protocol.h). Numeric because
    // this header stays free of wire dependencies; the regression suite
    // cross-checks every enumerator against this mapping so a new command
    // cannot silently fall into UNKNOWN.
    switch (command) {
        case 10: // SERVICE_CMD_LOGON_HANDOFF
            return SERVICE_IPC_CLASS_HANDOFF;
        case 5:  // SERVICE_CMD_RESET
        case 18: // SERVICE_CMD_INSTALL_UPDATE
        case 19: // SERVICE_CMD_SET_UPDATE_POLICY
            return SERVICE_IPC_CLASS_LIFECYCLE;
        case 7:  // SERVICE_CMD_WRITE_LOG_SNAPSHOT
        case 8:  // SERVICE_CMD_WRITE_JSON_SNAPSHOT
        case 9:  // SERVICE_CMD_WRITE_PROBE_REPORT
            return SERVICE_IPC_CLASS_BULK_OUTPUT;
        case 1:  // SERVICE_CMD_PING
        case 2:  // SERVICE_CMD_GET_SNAPSHOT
        case 3:  // SERVICE_CMD_GET_TELEMETRY
        case 4:  // SERVICE_CMD_APPLY
        case 6:  // SERVICE_CMD_GET_ACTIVE_DESIRED
        case 11: // SERVICE_CMD_GET_OPERATION_RESULT
        case 12: // SERVICE_CMD_GET_STARTUP_POLICY
        case 13: // SERVICE_CMD_SET_STARTUP_POLICY
        case 14: // SERVICE_CMD_REFRESH_STARTUP_PROFILE
        case 15: // SERVICE_CMD_RESUME_RESTORE
        case 16: // SERVICE_CMD_GET_UPDATE_STATE
        case 17: // SERVICE_CMD_CHECK_FOR_UPDATE
            return SERVICE_IPC_CLASS_NORMAL;
        default:
            return SERVICE_IPC_CLASS_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Token buckets and the fixed-size admission table.
// ---------------------------------------------------------------------------
struct ServiceIpcBucket {
    unsigned long long tokensMilli;
    unsigned long long lastRefillMs;

    void refill(unsigned long long nowMs, unsigned long long burstMilli,
                unsigned long long perSecondMilli) {
        if (!service_ipc_time_at_or_after(nowMs, lastRefillMs)) {
            // Clock went backwards (host resume with a non-monotonic source):
            // keep the accumulated tokens, just move the anchor forward.
            lastRefillMs = nowMs;
            return;
        }
        unsigned long long elapsedMs = nowMs - lastRefillMs;
        if (elapsedMs > 0 && tokensMilli < burstMilli) {
            // Cap the multiplication window so long idle cannot overflow.
            if (elapsedMs > 3600ULL * 1000ULL) elapsedMs = 3600ULL * 1000ULL;
            tokensMilli += elapsedMs * perSecondMilli / 1000ULL;
            if (tokensMilli > burstMilli) tokensMilli = burstMilli;
        } else if (tokensMilli > burstMilli) {
            tokensMilli = burstMilli;
        }
        lastRefillMs = nowMs;
    }

    bool spend(unsigned long long costMilli) {
        if (tokensMilli < costMilli) return false;
        tokensMilli -= costMilli;
        return true;
    }
};

enum ServiceIpcAdmissionDecision {
    SERVICE_IPC_ADMITTED = 0,
    SERVICE_IPC_REJECTED_RATE = 1,
    SERVICE_IPC_REJECTED_CAPACITY = 2,
};

struct ServiceIpcIdentitySlot {
    bool used;
    unsigned int hash;
    unsigned long long lastSeenMs;
    ServiceIpcThrottleKey key;
    ServiceIpcBucket normalBucket;
    ServiceIpcBucket handoffBucket;
};

struct ServiceIpcAdmissionTable {
    ServiceIpcIdentitySlot slots[SERVICE_IPC_TABLE_SLOTS];
    ServiceIpcBucket anonymousBucket;

    void reset(unsigned long long nowMs) {
        memset(this, 0, sizeof(*this));
        anonymousBucket.tokensMilli = SERVICE_IPC_ANON_BUDGET_MAX * 1000ULL;
        anonymousBucket.lastRefillMs = nowMs;
    }

    // Returns the resident entry or nullptr when absent. Expired entries are
    // treated as absent by callers through acquire().
    ServiceIpcIdentitySlot* find(const ServiceIpcThrottleKey& key,
                                 unsigned int keyHash) {
        if (!key.valid) return nullptr;
        unsigned int mask = SERVICE_IPC_TABLE_SLOTS - 1;
        unsigned int index = keyHash & mask;
        for (unsigned int probe = 0; probe < SERVICE_IPC_TABLE_SLOTS; ++probe) {
            ServiceIpcIdentitySlot* slot = &slots[index];
            if (!slot->used) return nullptr;
            if (slot->hash == keyHash && slot->key.equals(key)) return slot;
            index = (index + 1) & mask;
        }
        return nullptr;
    }

    // Returns a live (or freshly installed / idle-evicted / LRU-evicted)
    // entry. A full table evicts the least-recently-seen slot, so nobody can
    // be locked out permanently.
    ServiceIpcIdentitySlot* acquire(const ServiceIpcThrottleKey& key,
                                    unsigned int keyHash,
                                    unsigned long long nowMs) {
        if (!key.valid) return nullptr;
        unsigned int mask = SERVICE_IPC_TABLE_SLOTS - 1;
        unsigned int index = keyHash & mask;
        ServiceIpcIdentitySlot* evictCandidate = nullptr;
        for (unsigned int probe = 0; probe < SERVICE_IPC_TABLE_SLOTS; ++probe) {
            ServiceIpcIdentitySlot* slot = &slots[index];
            if (!slot->used) return install(slot, key, keyHash, nowMs);
            if (slot->hash == keyHash && slot->key.equals(key)) {
                slot->lastSeenMs = nowMs;
                return slot;
            }
            if (service_ipc_time_at_or_after(nowMs,
                    slot->lastSeenMs + SERVICE_IPC_IDLE_EXPIRY_MS)) {
                return install(slot, key, keyHash, nowMs);
            }
            if (!evictCandidate ||
                service_ipc_time_at_or_after(evictCandidate->lastSeenMs,
                                             slot->lastSeenMs)) {
                evictCandidate = slot;
            }
            index = (index + 1) & mask;
        }
        return install(evictCandidate, key, keyHash, nowMs);
    }

private:
    ServiceIpcIdentitySlot* install(ServiceIpcIdentitySlot* slot,
                                    const ServiceIpcThrottleKey& key,
                                    unsigned int keyHash,
                                    unsigned long long nowMs) {
        bool reuseResident = slot->used && slot->hash == keyHash &&
                             slot->key.equals(key);
        if (!reuseResident) {
            memset(slot, 0, sizeof(*slot));
            slot->key = key;
            slot->hash = keyHash;
            // Fresh identities start with full buckets.
            slot->normalBucket.tokensMilli = SERVICE_IPC_BUCKET_BURST_MILLI;
            slot->handoffBucket.tokensMilli = SERVICE_IPC_HANDOFF_RESERVE_MILLI;
        }
        slot->used = true;
        slot->lastSeenMs = nowMs;
        slot->normalBucket.lastRefillMs = nowMs;
        slot->handoffBucket.lastRefillMs = nowMs;
        return slot;
    }
};

// Ask whether a request of `cls` from `key` may proceed at time `nowMs`.
inline ServiceIpcAdmissionDecision service_ipc_decide_admission(
        ServiceIpcAdmissionTable* table, const ServiceIpcThrottleKey& key,
        ServiceIpcRequestClass cls, unsigned long long nowMs) {
    if (cls == SERVICE_IPC_CLASS_UNKNOWN) return SERVICE_IPC_REJECTED_CAPACITY;
    if (!table || !key.valid) return SERVICE_IPC_ADMITTED;
    ServiceIpcIdentitySlot* slot =
        table->find(key, service_ipc_key_hash(key));
    if (!slot ||
        service_ipc_time_at_or_after(nowMs,
            slot->lastSeenMs + SERVICE_IPC_IDLE_EXPIRY_MS)) {
        // Fresh (or expired) identity: admit. The entry is installed when the
        // outcome is charged, so an unidentifiable flood gains nothing.
        return SERVICE_IPC_ADMITTED;
    }
    unsigned long long needed = (unsigned long long)SERVICE_IPC_COST_SUCCESS *
                                1000ULL;
    const ServiceIpcBucket& bucket = cls == SERVICE_IPC_CLASS_HANDOFF
                                         ? slot->handoffBucket
                                         : slot->normalBucket;
    return bucket.tokensMilli >= needed ? SERVICE_IPC_ADMITTED
                                        : SERVICE_IPC_REJECTED_RATE;
}

// Apply the cost of an observed outcome at time `nowMs`. `cls` routes the
// charge to the reserved handoff bucket or the shared normal bucket. An
// invalid key charges only the small global anonymous budget.
inline void service_ipc_charge(ServiceIpcAdmissionTable* table,
                               const ServiceIpcThrottleKey& key,
                               ServiceIpcRequestClass cls,
                               unsigned int costTokens,
                               unsigned long long nowMs) {
    if (!table) return;
    unsigned long long costMilli = (unsigned long long)costTokens * 1000ULL;
    if (!key.valid) {
        table->anonymousBucket.refill(nowMs,
            SERVICE_IPC_ANON_BUDGET_MAX * 1000ULL,
            SERVICE_IPC_ANON_REFILL_PER_SECOND_MILLI);
        table->anonymousBucket.spend(costMilli);
        return;
    }
    ServiceIpcIdentitySlot* slot =
        table->acquire(key, service_ipc_key_hash(key), nowMs);
    if (!slot) return;
    if (cls == SERVICE_IPC_CLASS_HANDOFF) {
        slot->handoffBucket.refill(nowMs, SERVICE_IPC_HANDOFF_RESERVE_MILLI,
            SERVICE_IPC_HANDOFF_REFILL_PER_SECOND_MILLI);
        slot->handoffBucket.spend(costMilli);
    } else {
        slot->normalBucket.refill(nowMs, SERVICE_IPC_BUCKET_BURST_MILLI,
            SERVICE_IPC_REFILL_PER_SECOND_MILLI);
        slot->normalBucket.spend(costMilli);
    }
}

#endif // GREEN_CURVE_SERVICE_IPC_THROTTLE_POLICY_H
