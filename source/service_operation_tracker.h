// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_SERVICE_OPERATION_TRACKER_H
#define GREEN_CURVE_SERVICE_OPERATION_TRACKER_H

#define SERVICE_OPERATION_HISTORY_CAPACITY 16

struct ServiceOperationRecord {
    gc_u64 operationId;
    gc_u32 state;
    gc_u32 responseStatus;
    char message[512];
};

struct ServiceOperationTracker {
    ServiceOperationRecord records[SERVICE_OPERATION_HISTORY_CAPACITY];
    unsigned int nextRecord;
};

enum ServiceOperationBeginResult {
    SERVICE_OPERATION_BEGIN_INVALID = 0,
    SERVICE_OPERATION_BEGIN_STARTED = 1,
    SERVICE_OPERATION_BEGIN_DUPLICATE = 2,
};

static inline const ServiceOperationRecord* service_operation_find(
    const ServiceOperationTracker* tracker, gc_u64 operationId) {
    if (!tracker || operationId == 0) return nullptr;
    for (unsigned int i = 0; i < SERVICE_OPERATION_HISTORY_CAPACITY; i++) {
        if (tracker->records[i].operationId == operationId) {
            return &tracker->records[i];
        }
    }
    return nullptr;
}

static inline ServiceOperationRecord* service_operation_find_mutable(
    ServiceOperationTracker* tracker, gc_u64 operationId) {
    return const_cast<ServiceOperationRecord*>(service_operation_find(
        tracker, operationId));
}

static inline ServiceOperationBeginResult service_operation_begin(
    ServiceOperationTracker* tracker, gc_u64 operationId,
    const ServiceOperationRecord** existingOut) {
    if (existingOut) *existingOut = nullptr;
    if (!tracker || operationId == 0) return SERVICE_OPERATION_BEGIN_INVALID;
    const ServiceOperationRecord* existing = service_operation_find(
        tracker, operationId);
    if (existing) {
        if (existingOut) *existingOut = existing;
        return SERVICE_OPERATION_BEGIN_DUPLICATE;
    }
    unsigned int index = tracker->nextRecord %
        SERVICE_OPERATION_HISTORY_CAPACITY;
    tracker->nextRecord = (index + 1) % SERVICE_OPERATION_HISTORY_CAPACITY;
    ServiceOperationRecord* record = &tracker->records[index];
    memset(record, 0, sizeof(*record));
    record->operationId = operationId;
    record->state = SERVICE_OPERATION_IN_PROGRESS;
    record->responseStatus = SERVICE_STATUS_ERROR;
    return SERVICE_OPERATION_BEGIN_STARTED;
}

static inline bool service_operation_complete(ServiceOperationTracker* tracker,
    gc_u64 operationId, gc_u32 responseStatus, const char* message) {
    ServiceOperationRecord* record = service_operation_find_mutable(tracker,
        operationId);
    if (!record || record->state != SERVICE_OPERATION_IN_PROGRESS) return false;
    record->responseStatus = responseStatus;
    record->state = responseStatus == SERVICE_STATUS_OK
        ? SERVICE_OPERATION_SUCCEEDED : SERVICE_OPERATION_FAILED;
    gc_strlcpy(record->message, sizeof(record->message),
        message ? message : "");
    return true;
}

static inline bool service_operation_restore(ServiceOperationTracker* tracker,
    gc_u64 operationId, gc_u32 state, gc_u32 responseStatus,
    const char* message) {
    if (!tracker || operationId == 0 ||
        state < SERVICE_OPERATION_IN_PROGRESS ||
        state > SERVICE_OPERATION_OUTCOME_UNKNOWN) return false;
    const ServiceOperationRecord* existing = nullptr;
    ServiceOperationBeginResult begin = service_operation_begin(tracker,
        operationId, &existing);
    ServiceOperationRecord* record = service_operation_find_mutable(tracker,
        operationId);
    if (begin == SERVICE_OPERATION_BEGIN_INVALID || !record) return false;
    record->state = state;
    record->responseStatus = responseStatus;
    gc_strlcpy(record->message, sizeof(record->message),
        message ? message : "");
    return true;
}

#endif // GREEN_CURVE_SERVICE_OPERATION_TRACKER_H
