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

#endif
