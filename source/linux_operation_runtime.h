// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

class LinuxOperationRequestGuard {
public:
    LinuxOperationRequestGuard(const ServiceRequest* request,
        ServiceResponse* response, const char* name)
        : request_(request), response_(response), name_(name), started_(false),
          execute_(false), startedAt_(monotonic_ms()) {
        response_->operationId = request_->operationId;
        const ServiceOperationRecord* existing = nullptr;
        ServiceOperationBeginResult begin = service_operation_begin(
            &g_operationTracker, request_->operationId, &existing);
        if (begin == SERVICE_OPERATION_BEGIN_STARTED) {
            started_ = true;
            execute_ = true;
            response_->operationState = SERVICE_OPERATION_IN_PROGRESS;
            dlog("daemon operation: id=%llu command=%s state=in-progress\n",
                (unsigned long long)request_->operationId, name_);
            persist_daemon_operation(request_->operationId,
                SERVICE_OPERATION_IN_PROGRESS, SERVICE_STATUS_ERROR,
                SERVICE_OUTCOME_SEVERITY_ERROR, "operation started");
        } else if (begin == SERVICE_OPERATION_BEGIN_DUPLICATE && existing) {
            response_->operationState = existing->state;
            response_->status = existing->responseStatus;
            // Same rule as the Windows guard: a replayed answer keeps the
            // severity the original carried.
            response_->outcomeSeverity = existing->outcomeSeverity;
            gc_strlcpy(response_->message, sizeof(response_->message),
                existing->message[0] ? existing->message :
                "operation result cached");
            dlog("daemon operation: id=%llu command=%s deduplicated state=%u\n",
                (unsigned long long)request_->operationId, name_,
                (unsigned int)existing->state);
        } else {
            response_->status = SERVICE_STATUS_ERROR;
            response_->operationState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
            gc_strlcpy(response_->message, sizeof(response_->message),
                "mutation request is missing a valid operation ID");
        }
    }
    ~LinuxOperationRequestGuard() {
        if (!started_) return;
        // Resolved before the record is written for the same reason as on
        // Windows: this record is what a retry replays.
        response_->outcomeSeverity = service_response_resolve_outcome_severity(
            response_->status, response_->outcomeSeverity);
        service_operation_complete(&g_operationTracker, request_->operationId,
            response_->status, response_->outcomeSeverity, response_->message);
        response_->operationState = response_->status == SERVICE_STATUS_OK
            ? SERVICE_OPERATION_SUCCEEDED : SERVICE_OPERATION_FAILED;
        persist_daemon_operation(request_->operationId,
            response_->operationState, response_->status,
            response_->outcomeSeverity, response_->message);
        dlog("daemon operation: id=%llu command=%s state=%s severity=%s durationMs=%llu\n",
            (unsigned long long)request_->operationId, name_,
            response_->status == SERVICE_STATUS_OK ? "succeeded" : "failed",
            service_outcome_severity_name(response_->outcomeSeverity),
            monotonic_ms() - startedAt_);
    }
    bool execute() const { return execute_; }
private:
    const ServiceRequest* request_;
    ServiceResponse* response_;
    const char* name_;
    bool started_;
    bool execute_;
    unsigned long long startedAt_;
};
