// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_COMMANDS_WINDOWS_H
#define GREEN_CURVE_PROFILE_COMMANDS_WINDOWS_H
#include "profile_commands.h"
#include "profile_legacy_document.h"
#include "profile_file_read.h"
#include "profile_capture_commands.h"
#include "profile_check_commands.h"

struct WindowsProfileCommands {
    bool capture(gc_u32 domains, HardwareCaptureObservation& output, ProfileCodecError& error) {
        ServiceRequest request{}; auto response = std::make_unique<ServiceResponse>();
        request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_CAPTURE_CURRENT; request.captureDomains = domains;
        request.callerPid = GetCurrentProcessId(); ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
        StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client passive capture");
        if (!service_send_request(&request, response.get(), (DWORD)service_state_read_response_timeout_ms(true), error.message, sizeof(error.message))) return false;
        if (response->status != SERVICE_STATUS_OK || !response->capture.valid)
            return profile_codec::fail(&error, 0, response->message[0] ? response->message : "capture is unavailable");
        output = response->capture; return true;
    }
    bool distinct(const std::string& config, const std::string& file, ProfileCodecError& error) {
        char a[MAX_PATH]{}, b[MAX_PATH]{};
        const DWORD na = gc_GetFullPathNameUtf8(config.c_str(), sizeof(a), a, nullptr);
        const DWORD nb = gc_GetFullPathNameUtf8(file.c_str(), sizeof(b), b, nullptr);
        GcWideUtf8Arg wa(a), wb(b);
        if (!na || na >= sizeof(a) || !nb || nb >= sizeof(b) || !wa.valid_for(a) || !wb.valid_for(b) ||
            CompareStringOrdinal(wa.value, -1, wb.value, -1, TRUE) == CSTR_EQUAL)
            return profile_codec::fail(&error, 0, "profile file must have a distinct valid destination path");
        return true;
    }
    bool read(const std::string& path, std::string& bytes, size_t maximum, bool missing, ProfileCodecError& error) {
        return profile_file_read(path.c_str(), bytes, maximum, missing, error);
    }
    bool write(const std::string& path, const std::string& bytes, ProfileCodecError& error) {
        return write_text_file_atomic(path.c_str(), bytes.data(), bytes.size(), error.message, sizeof(error.message));
    }
    bool legacy(const std::string& path, int slot, ProfileDocument& document, bool& found, ProfileCodecError& error) {
        found = is_profile_slot_saved(path.c_str(), slot);
        if (!found) return true;
        DesiredSettings desired{};
        return load_profile_from_config(path.c_str(), slot, &desired, error.message, sizeof(error.message), PROFILE_READ_FOR_OWNERSHIP) &&
            profile_document_from_legacy_desired(desired, document, error.message, sizeof(error.message));
    }
    bool commit(const std::string& path, const ProfileBankEdit& edit, ProfileCodecError& error) {
        ConfigStorageLockGuard lock;
        if (!lock.locked()) return profile_codec::fail(&error, 0, "could not acquire profile storage lock");
        const bool ok = profile_bank_commit_edit(edit,
            [&](std::string& bytes) { return read(path, bytes, profile_slot::MAX_CONFIG_BYTES, true, error); },
            [&](const std::string& bytes) { return write(path, bytes, error); }, &error);
        if (ok) {
            (void)gc_WritePrivateProfileStringUtf8(nullptr, nullptr, nullptr, path.c_str());
            invalidate_tray_profile_cache();
        }
        debug_log("profile import: committed=%d records=%d\n", ok ? 1 : 0, edit.count);
        return ok;
    }
    void message(const std::string& text) { gc_cli_console_write(text.c_str()); }
    bool confirm() {
        message("Type IMPORT to commit, or anything else to cancel: ");
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        bool own = false;
        if (!input || input == INVALID_HANDLE_VALUE || GetFileType(input) == FILE_TYPE_UNKNOWN) {
            if (!g_cliConsoleIsInteractive) return false;
            input = CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            own = input != INVALID_HANDLE_VALUE;
        }
        char answer[16]{};
        size_t count = 0; bool complete = false;
        while (count + 1 < sizeof(answer)) {
            char c = 0; DWORD read = 0;
            if (!ReadFile(input, &c, 1, &read, nullptr) || read != 1) break;
            if (c == '\n') { complete = true; break; }
            answer[count++] = c;
        }
        if (own) CloseHandle(input);
        if (count && answer[count - 1] == '\r') answer[count - 1] = 0;
        return complete && !strcmp(answer, "IMPORT");
    }
};

static bool profile_commands_windows(LPWSTR commandLine, int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine, &argc);
    if (!argv) return false;
    bool transfer = false, exporting = false;
    for (int i = 1; i < argc; ++i) {
        exporting |= !wcscmp(argv[i], L"--export-profiles");
        transfer |= exporting || !wcscmp(argv[i], L"--import-profiles") || !wcscmp(argv[i], L"--capture-current") || !wcscmp(argv[i], L"--check-profile");
    }
    if (!transfer) { LocalFree(argv); return false; }
    gc_cli_console_ready();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        char value[4096]{};
        if (!gc_wide_to_utf8(argv[i], value, sizeof(value), nullptr)) {
            LocalFree(argv); exitCode = PROFILE_COMMAND_INVALID; gc_cli_console_write("Invalid profile command text.\n"); return true;
        }
        args.emplace_back(value);
    }
    LocalFree(argv);
    WindowsProfileCommands io;
    if (profile_check_commands_run(args, io, exitCode)) return true;
    if (profile_capture_commands_run(args, g_app.configPath, io, exitCode)) return true;
    // Historical readers use the INI API. Keep the complete export generation
    // locked while those readers decode it; new documents use the raw snapshot.
    if (exporting) {
        ConfigStorageLockGuard lock;
        if (!lock.locked()) { exitCode = PROFILE_COMMAND_IO; return true; }
        return profile_commands_run(args, g_app.configPath, io, exitCode);
    }
    return profile_commands_run(args, g_app.configPath, io, exitCode);
}
#endif
