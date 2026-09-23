// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Included by installer_apply.cpp. All recoverable state is captured before
// the old service is stopped. A failed rollback retains its protected copies
// and reports their location rather than discarding the only working binaries.

#include <string>
#include <vector>
#include "installer_transaction_files.h"

static const WCHAR* const gc_registration_values[] = {
    L"DisplayName", L"DisplayVersion", L"Publisher", L"InstallLocation",
    L"UninstallString", L"QuietUninstallString", L"DisplayIcon", L"NoModify",
    L"NoRepair", L"EstimatedSize", L"GreenCurveStartMenuShortcut",
    L"GreenCurveDesktopShortcut", GC_SETUP_SETTINGS_EXPORT_VALUE,
};

struct GcSavedRegistryValue {
    bool present = false;
    DWORD type = 0;
    std::vector<BYTE> data;
};

struct GcInstallTransaction {
    GcInstallContext* context;
    WCHAR target[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR scratch[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR oldFolder[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR names[GC_ARCHIVE_MAX_FILES][GC_ARCHIVE_MAX_NAME + 1] = {};
    bool oldFile[GC_ARCHIVE_MAX_FILES] = {};
    bool changedFile[GC_ARCHIVE_MAX_FILES] = {};
    bool registryPresent = false;
    bool servicePresent = false;
    bool serviceWasRunning = false;
    DWORD serviceStartType = SERVICE_NO_CHANGE;
    std::wstring serviceCommand;
    std::wstring serviceDisplayName;
    WCHAR previousServiceDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR previousServiceBinary[GC_INSTALLER_MAX_PATH_CHARS] = {};
    bool previousDirectoryHardened = false;
    bool previousBinaryHardened = false;
    std::vector<BYTE> failureActions;
    std::vector<BYTE> failureFlag;
    std::vector<BYTE> description;
    GcSavedRegistryValue values[GC_ARRAY_COUNT(gc_registration_values)];

    explicit GcInstallTransaction(GcInstallContext* owner) : context(owner) {}

    bool paths(uint32_t index, WCHAR* finalPath, WCHAR* stagedPath,
               WCHAR* backupPath) const {
        return gc_join_path(target, names[index], finalPath, GC_INSTALLER_MAX_PATH_CHARS) &&
               gc_join_path(scratch, names[index], stagedPath, GC_INSTALLER_MAX_PATH_CHARS) &&
               gc_join_path(oldFolder, names[index], backupPath, GC_INSTALLER_MAX_PATH_CHARS);
    }

    static bool query_extra(SC_HANDLE service, DWORD level, std::vector<BYTE>* out) {
        DWORD needed = 0;
        QueryServiceConfig2W(service, level, nullptr, 0, &needed);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) return false;
        out->resize(needed);
        return QueryServiceConfig2W(service, level, out->data(), needed, &needed) != FALSE;
    }

    bool snapshot_service() {
        GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.valid()) {
            gc_set_error(context, "Could not inspect the service before the upgrade (error %lu).",
                         GetLastError());
            return false;
        }
        GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME,
            SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
        if (!service.valid()) {
            DWORD error = GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            gc_set_error(context, "Could not inspect the service before the upgrade (error %lu).", error);
            return false;
        }
        servicePresent = true;
        DWORD needed = 0;
        QueryServiceConfigW(service.get(), nullptr, 0, &needed);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
            gc_set_error(context, "Could not size the previous service registration (error %lu).",
                         GetLastError());
            return false;
        }
        std::vector<BYTE> configBytes(needed);
        auto* config = (QUERY_SERVICE_CONFIGW*)configBytes.data();
        if (!QueryServiceConfigW(service.get(), config, needed, &needed) ||
            !config->lpBinaryPathName || !config->lpDisplayName ||
            !query_extra(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions) ||
            !query_extra(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failureFlag) ||
            !query_extra(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)) {
            gc_set_error(context, "Could not save the previous service configuration (error %lu).",
                         GetLastError());
            return false;
        }
        serviceCommand = config->lpBinaryPathName;
        serviceDisplayName = config->lpDisplayName;
        serviceStartType = config->dwStartType;
        int argCount = 0;
        LPWSTR* args = CommandLineToArgvW(serviceCommand.c_str(), &argCount);
        if (args && argCount > 0 &&
            SUCCEEDED(StringCchCopyW(previousServiceBinary,
                GC_ARRAY_COUNT(previousServiceBinary), args[0])) &&
            gc_directory_of(previousServiceBinary, previousServiceDirectory,
                            GC_ARRAY_COUNT(previousServiceDirectory))) {
            previousDirectoryHardened = service_path_dacl_is_ours(
                previousServiceDirectory, GC_SERVICE_ACL_DIRECTORY);
            previousBinaryHardened = service_path_dacl_is_ours(
                previousServiceBinary, GC_SERVICE_ACL_BINARY);
        }
        if (args) LocalFree(args);
        SERVICE_STATUS_PROCESS status = {};
        if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                                  sizeof(status), &needed)) {
            gc_set_error(context, "Could not save the previous service state (error %lu).",
                         GetLastError());
            return false;
        }
        serviceWasRunning = status.dwCurrentState != SERVICE_STOPPED;
        gc_log_step("transaction: saved service configuration, running=%d",
                    serviceWasRunning ? 1 : 0);
        return true;
    }

    bool snapshot_registry() {
        HKEY key = nullptr;
        LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, 0,
                                    KEY_QUERY_VALUE, &key);
        if (status == ERROR_FILE_NOT_FOUND) return true;
        if (status != ERROR_SUCCESS) {
            gc_set_error(context, "Could not save the uninstall record (error %ld).", status);
            return false;
        }
        registryPresent = true;
        bool ok = true;
        for (size_t i = 0; i < GC_ARRAY_COUNT(gc_registration_values); ++i) {
            DWORD bytes = 0;
            status = RegQueryValueExW(key, gc_registration_values[i], nullptr,
                                      &values[i].type, nullptr, &bytes);
            if (status == ERROR_FILE_NOT_FOUND) continue;
            if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) { ok = false; break; }
            values[i].data.resize(bytes);
            status = RegQueryValueExW(key, gc_registration_values[i], nullptr,
                &values[i].type, bytes ? values[i].data.data() : nullptr, &bytes);
            if (status != ERROR_SUCCESS) { ok = false; break; }
            values[i].present = true;
        }
        RegCloseKey(key);
        if (!ok) gc_set_error(context, "Could not save all uninstall values (error %ld).", status);
        return ok;
    }

    bool prepare(const WCHAR* directory) {
        if (FAILED(StringCchCopyW(target, GC_ARRAY_COUNT(target), directory)) ||
            !snapshot_service() || !snapshot_registry()) return false;
        if (!gc_create_private_temp_directory(scratch, GC_ARRAY_COUNT(scratch)) ||
            !gc_join_path(scratch, L"old", oldFolder, GC_ARRAY_COUNT(oldFolder)) ||
            !CreateDirectoryW(oldFolder, nullptr)) {
            gc_set_error(context, "Could not prepare protected upgrade storage (error %lu).",
                         GetLastError());
            cleanup();
            return false;
        }
        for (uint32_t i = 0; i < context->payload.fileCount; ++i) {
            const GcPayloadFile* file = &context->payload.files[i];
            if (!gc_utf8_to_wide(file->name, names[i], (int)GC_ARRAY_COUNT(names[i]))) {
                gc_set_error(context, "Could not decode payload file %hs.", file->name);
                cleanup();
                return false;
            }
            WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            WCHAR stagedPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            WCHAR backupPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            if (!paths(i, finalPath, stagedPath, backupPath) ||
                !gc_write_payload_file(scratch, file, context)) {
                if (!context->error[0]) gc_set_error(context, "An upgrade path is too long.");
                cleanup();
                return false;
            }
            GcScopedHandle existing(CreateFileW(finalPath, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!existing.valid()) {
                DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND) continue;
                gc_set_error(context, "Could not inspect %ls before replacement (error %lu).",
                             finalPath, error);
                cleanup();
                return false;
            }
            BY_HANDLE_FILE_INFORMATION info = {};
            if (!GetFileInformationByHandle(existing.get(), &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                info.nNumberOfLinks != 1) {
                gc_set_error(context, "Existing file %ls is not an ordinary single-link file.", finalPath);
                cleanup();
                return false;
            }
            if (!CopyFileW(finalPath, backupPath, TRUE)) {
                gc_set_error(context, "Could not save %ls before replacement (error %lu).",
                             finalPath, GetLastError());
                cleanup();
                return false;
            }
            oldFile[i] = true;
        }
        gc_log_step("transaction: staged %lu payload files and saved previous target files",
                    (unsigned long)context->payload.fileCount);
        return true;
    }

    bool replace(uint32_t index) {
        WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
        WCHAR stagedPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
        WCHAR backupPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!paths(index, finalPath, stagedPath, backupPath)) return false;
        HRESULT result = gc_replace_staged_install_file(stagedPath, finalPath);
        if (FAILED(result)) {
            gc_set_error(context, "Could not replace %ls (HRESULT 0x%08lx).",
                         finalPath, (unsigned long)result);
            return false;
        }
        changedFile[index] = true;
        gc_log_step("transaction: replaced %ls", finalPath);
        return true;
    }

    bool restore_registry() {
        HKEY key = nullptr;
        LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) { gc_log_fail("rollback: open uninstall record error %ld", status); return false; }
        bool ok = true;
        for (size_t i = 0; i < GC_ARRAY_COUNT(gc_registration_values); ++i) {
            const GcSavedRegistryValue& saved = values[i];
            status = saved.present
                ? RegSetValueExW(key, gc_registration_values[i], 0, saved.type,
                    saved.data.empty() ? nullptr : saved.data.data(), (DWORD)saved.data.size())
                : RegDeleteValueW(key, gc_registration_values[i]);
            if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !saved.present)) {
                gc_log_fail("rollback: restore uninstall value %ls error %ld",
                            gc_registration_values[i], status);
                ok = false;
            }
        }
        RegCloseKey(key);
        if (!registryPresent && ok) {
            status = RegDeleteKeyW(HKEY_LOCAL_MACHINE, GC_SETUP_UNINSTALL_KEY);
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
                gc_log_fail("rollback: remove new uninstall key error %ld", status);
                ok = false;
            }
        }
        return ok;
    }

    bool restore_files() {
        return gc_restore_install_files(context->payload.fileCount,
            [&](uint32_t i) { return changedFile[i]; },
            [&](uint32_t i) { return oldFile[i]; },
            [&](uint32_t i) {
                WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                WCHAR stagedPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                WCHAR backupPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                if (!paths(i, finalPath, stagedPath, backupPath)) return false;
                HRESULT result = gc_restore_previous_install_file(backupPath, finalPath);
                if (FAILED(result)) {
                    gc_log_fail("rollback: restore %ls HRESULT 0x%08lx",
                                finalPath, (unsigned long)result);
                    return false;
                }
                return true;
            },
            [&](uint32_t i) {
                WCHAR finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                WCHAR stagedPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                WCHAR backupPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
                if (!paths(i, finalPath, stagedPath, backupPath)) return false;
                if (DeleteFileW(finalPath) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
                gc_log_fail("rollback: remove new %ls error %lu", finalPath, GetLastError());
                return false;
            });
    }

    bool restore_service() {
        GcScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.valid()) return false;
        GcScopedServiceHandle service(OpenServiceW(scm.get(), GC_SETUP_SERVICE_NAME,
            SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS | DELETE));
        if (!service.valid()) {
            if (!servicePresent && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            gc_log_fail("rollback: open service error %lu", GetLastError());
            return false;
        }
        if (!servicePresent) {
            if (!DeleteService(service.get())) {
                gc_log_fail("rollback: remove new service error %lu", GetLastError());
                return false;
            }
            return true;
        }
        bool ok = ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, serviceStartType,
            SERVICE_NO_CHANGE, serviceCommand.c_str(), nullptr, nullptr, nullptr, nullptr,
            nullptr, serviceDisplayName.c_str()) != FALSE;
        if (!ok) gc_log_fail("rollback: restore service command error %lu", GetLastError());
        if (!failureActions.empty() && !ChangeServiceConfig2W(service.get(),
            SERVICE_CONFIG_FAILURE_ACTIONS, failureActions.data())) {
            gc_log_fail("rollback: restore service recovery error %lu", GetLastError()); ok = false;
        }
        if (!failureFlag.empty() && !ChangeServiceConfig2W(service.get(),
            SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, failureFlag.data())) {
            gc_log_fail("rollback: restore service recovery flag error %lu", GetLastError()); ok = false;
        }
        if (!description.empty() && !ChangeServiceConfig2W(service.get(),
            SERVICE_CONFIG_DESCRIPTION, description.data())) {
            gc_log_fail("rollback: restore service description error %lu", GetLastError()); ok = false;
        }
        // A successful helper may have released the old location before the
        // parent's SCM read-back failed. Restore only DACLs proven to be ours
        // in the preflight snapshot, using pinned handles (no name-based write).
        if (previousServiceDirectory[0] && previousDirectoryHardened) {
            GcScopedHandle directory(CreateFileW(previousServiceDirectory,
                FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            char aclError[160] = {};
            if (!directory.valid() || !apply_protected_service_dacl_to_handle(
                    directory.get(), GC_SERVICE_ACL_DIRECTORY, true,
                    aclError, sizeof(aclError))) {
                gc_log_fail("rollback: restore previous directory protection failed: %s",
                            aclError[0] ? aclError : "could not open the directory");
                ok = false;
            }
        }
        if (previousServiceBinary[0] && previousBinaryHardened) {
            GcScopedHandle binary(CreateFileW(previousServiceBinary,
                FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            char aclError[160] = {};
            if (!binary.valid() || !apply_protected_service_dacl_to_handle(
                    binary.get(), GC_SERVICE_ACL_BINARY, true,
                    aclError, sizeof(aclError))) {
                gc_log_fail("rollback: restore previous binary protection failed: %s",
                            aclError[0] ? aclError : "could not open the binary");
                ok = false;
            }
        }
        if (ok && serviceWasRunning) {
            LPCWSTR arguments[] = {L"--manual"};
            if (!StartServiceW(service.get(), 1, arguments) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                gc_log_fail("rollback: restart previous service error %lu", GetLastError());
                ok = false;
            } else {
                // StartService only acknowledges the request. Confirm the
                // service actually reaches RUNNING before reporting recovery.
                const ULONGLONG started = GetTickCount64();
                GcScmWaitTracker tracker = {};
                gc_scm_wait_begin(&tracker, started);
                for (;;) {
                    SERVICE_STATUS_PROCESS state = {};
                    DWORD needed = 0;
                    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                        (LPBYTE)&state, sizeof(state), &needed)) {
                        gc_log_fail("rollback: query restarted service error %lu", GetLastError());
                        ok = false;
                        break;
                    }
                    int verdict = gc_scm_wait_step(&tracker, GetTickCount64(),
                        state.dwCurrentState, state.dwCheckPoint, state.dwWaitHint,
                        SERVICE_RUNNING);
                    if (verdict == GC_SCM_WAIT_REACHED) break;
                    if (verdict != GC_SCM_WAIT_CONTINUE) {
                        gc_log_fail("rollback: previous service did not resume (%s, state %lu, error %lu/%lu)",
                            gc_scm_wait_verdict_name(verdict),
                            (unsigned long)state.dwCurrentState,
                            (unsigned long)state.dwWin32ExitCode,
                            (unsigned long)state.dwServiceSpecificExitCode);
                        ok = false;
                        break;
                    }
                    Sleep(gc_scm_wait_poll_interval_ms(state.dwWaitHint));
                }
            }
        }
        return ok;
    }

    bool rollback(bool recordMayHaveChanged) {
        gc_log_step("transaction: rolling back failed installation");
        // A failed service helper may have restarted the previous registration
        // against the complete new payload. Stop it before touching any image.
        bool ignored = false;
        if (!gc_stop_service(context, &ignored)) {
            gc_log_fail("rollback: cannot prove the service process exited; preserving backups");
            return false;
        }
        bool files = restore_files();
        bool service = files && restore_service();
        bool record = !recordMayHaveChanged || restore_registry();
        bool ok = files && service && record;
        if (ok) {
            // The old service's ordinary shutdown can reset owned GPU state.
            // Only a prior binary known to understand the transfer command
            // can reapply the captured intent safely across protocol versions.
            if (gc_rollback_can_reapply_settings(serviceWasRunning,
                context->plan.captureFromInstalledBinary,
                context->haveCapturedSettings, previousServiceDirectory[0] != 0))
                gc_reapply_captured_settings(context, previousServiceDirectory);
            cleanup();
        }
        else gc_log_fail("rollback: incomplete; protected copies retained at %ls", scratch);
        return ok;
    }

    void cleanup() {
        for (uint32_t i = 0; i < context->payload.fileCount; ++i) {
            if (!names[i][0]) continue;
            WCHAR stagedPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            WCHAR backupPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
            if (scratch[0] && gc_join_path(scratch, names[i], stagedPath,
                                           GC_ARRAY_COUNT(stagedPath)) &&
                !DeleteFileW(stagedPath) && GetLastError() != ERROR_FILE_NOT_FOUND)
                gc_log_step("transaction: could not remove staged file %ls (error %lu)",
                            stagedPath, GetLastError());
            if (oldFolder[0] && gc_join_path(oldFolder, names[i], backupPath,
                                             GC_ARRAY_COUNT(backupPath)) &&
                !DeleteFileW(backupPath) && GetLastError() != ERROR_FILE_NOT_FOUND)
                gc_log_step("transaction: could not remove backup %ls (error %lu)",
                            backupPath, GetLastError());
        }
        if (oldFolder[0] && !RemoveDirectoryW(oldFolder) &&
            GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND)
            gc_log_step("transaction: protected backup folder remains at %ls (error %lu)",
                        oldFolder, GetLastError());
        if (scratch[0] && !RemoveDirectoryW(scratch) &&
            GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND)
            gc_log_step("transaction: protected staging folder remains at %ls (error %lu)",
                        scratch, GetLastError());
    }
};
