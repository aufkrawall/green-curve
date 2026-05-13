// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_WIN32_RAII_H
#define GREEN_CURVE_WIN32_RAII_H

#include "app_shared.h"

struct ScopedHandle {
    HANDLE handle;

    ScopedHandle() : handle(nullptr) {}
    explicit ScopedHandle(HANDLE value) : handle(value) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) { reset(); handle = other.handle; other.handle = nullptr; }
        return *this;
    }

    HANDLE get() const { return handle; }
    bool valid() const { return handle && handle != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return handle; }

    HANDLE detach() {
        HANDLE value = handle;
        handle = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (valid()) CloseHandle(handle);
        handle = value;
    }
};

struct ScopedGdiObject {
    HGDIOBJ object;

    ScopedGdiObject() : object(nullptr) {}
    explicit ScopedGdiObject(HGDIOBJ value) : object(value) {}
    ~ScopedGdiObject() { reset(); }

    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;

    ScopedGdiObject(ScopedGdiObject&& other) noexcept : object(other.object) { other.object = nullptr; }
    ScopedGdiObject& operator=(ScopedGdiObject&& other) noexcept {
        if (this != &other) { reset(); object = other.object; other.object = nullptr; }
        return *this;
    }

    HGDIOBJ get() const { return object; }
    bool valid() const { return object != nullptr; }
    operator HGDIOBJ() const { return object; }

    HGDIOBJ detach() {
        HGDIOBJ value = object;
        object = nullptr;
        return value;
    }

    void reset(HGDIOBJ value = nullptr) {
        if (object) DeleteObject(object);
        object = value;
    }
};

struct ScopedServiceHandle {
    SC_HANDLE handle;

    ScopedServiceHandle() : handle(nullptr) {}
    explicit ScopedServiceHandle(SC_HANDLE value) : handle(value) {}
    ~ScopedServiceHandle() { reset(); }

    ScopedServiceHandle(const ScopedServiceHandle&) = delete;
    ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;

    ScopedServiceHandle(ScopedServiceHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    ScopedServiceHandle& operator=(ScopedServiceHandle&& other) noexcept {
        if (this != &other) { reset(); handle = other.handle; other.handle = nullptr; }
        return *this;
    }

    SC_HANDLE get() const { return handle; }
    bool valid() const { return handle != nullptr; }
    operator SC_HANDLE() const { return handle; }

    SC_HANDLE detach() {
        SC_HANDLE value = handle;
        handle = nullptr;
        return value;
    }

    void reset(SC_HANDLE value = nullptr) {
        if (handle) CloseServiceHandle(handle);
        handle = value;
    }
};

struct ScopedCriticalSection {
    CRITICAL_SECTION* cs;
    explicit ScopedCriticalSection(CRITICAL_SECTION* cs) : cs(cs) { if (cs) EnterCriticalSection(cs); }
    ~ScopedCriticalSection() { if (cs) LeaveCriticalSection(cs); }
    ScopedCriticalSection(const ScopedCriticalSection&) = delete;
    ScopedCriticalSection& operator=(const ScopedCriticalSection&) = delete;
};

struct ScopedProcess {
    HANDLE processHandle;
    HANDLE threadHandle;
    HANDLE pipeRead;
    HANDLE pipeWrite;

    ScopedProcess() : processHandle(nullptr), threadHandle(nullptr), pipeRead(nullptr), pipeWrite(nullptr) {}
    ~ScopedProcess() { cleanup(); }

    ScopedProcess(const ScopedProcess&) = delete;
    ScopedProcess& operator=(const ScopedProcess&) = delete;

    ScopedProcess(ScopedProcess&& other) noexcept
        : processHandle(other.processHandle), threadHandle(other.threadHandle),
          pipeRead(other.pipeRead), pipeWrite(other.pipeWrite) {
        other.processHandle = nullptr; other.threadHandle = nullptr;
        other.pipeRead = nullptr; other.pipeWrite = nullptr;
    }

    ScopedProcess& operator=(ScopedProcess&& other) noexcept {
        if (this != &other) {
            cleanup();
            processHandle = other.processHandle; other.processHandle = nullptr;
            threadHandle = other.threadHandle; other.threadHandle = nullptr;
            pipeRead = other.pipeRead; other.pipeRead = nullptr;
            pipeWrite = other.pipeWrite; other.pipeWrite = nullptr;
        }
        return *this;
    }

    void assign(HANDLE proc, HANDLE thread) {
        cleanup();
        processHandle = proc;
        threadHandle = thread;
    }

    void assign_pipes(HANDLE read, HANDLE write) {
        if (pipeRead) CloseHandle(pipeRead);
        if (pipeWrite) CloseHandle(pipeWrite);
        pipeRead = read;
        pipeWrite = write;
    }

    bool valid() const { return processHandle != nullptr && processHandle != INVALID_HANDLE_VALUE; }

    DWORD wait(DWORD timeoutMs) const {
        if (!valid()) return WAIT_OBJECT_0;
        return WaitForSingleObject(processHandle, timeoutMs);
    }

    DWORD exit_code() const {
        if (!valid()) return 0;
        DWORD code = 0;
        GetExitCodeProcess(processHandle, &code);
        return code;
    }

    void terminate(DWORD exitCode = 1) {
        if (valid()) {
            TerminateProcess(processHandle, exitCode);
        }
    }

    void cleanup() {
        if (pipeWrite) { CloseHandle(pipeWrite); pipeWrite = nullptr; }
        if (pipeRead) { CloseHandle(pipeRead); pipeRead = nullptr; }
        if (threadHandle) { CloseHandle(threadHandle); threadHandle = nullptr; }
        if (processHandle) { CloseHandle(processHandle); processHandle = nullptr; }
    }
};

#endif
