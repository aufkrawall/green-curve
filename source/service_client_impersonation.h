// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_CLIENT_IMPERSONATION_H
#define GREEN_CURVE_SERVICE_CLIENT_IMPERSONATION_H

#include <cstdlib>

class ScopedServiceClientImpersonation {
public:
    explicit ScopedServiceClientImpersonation(HANDLE token)
        : active_(token && ImpersonateLoggedOnUser(token) != FALSE) {}
    ~ScopedServiceClientImpersonation() {
        if (active_ && !RevertToSelf()) {
            const DWORD error = GetLastError();
            debug_log("service file command: identity restoration failed error=%lu; terminating\n",
                (unsigned long)error);
            TerminateProcess(GetCurrentProcess(), ERROR_CANNOT_IMPERSONATE);
            std::abort();
        }
    }
    bool active() const { return active_; }
    ScopedServiceClientImpersonation(const ScopedServiceClientImpersonation&) = delete;
    ScopedServiceClientImpersonation& operator=(const ScopedServiceClientImpersonation&) = delete;
private:
    bool active_;
};

#endif
