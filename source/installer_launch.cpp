// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by installer_apply.cpp after its common helpers.

// ---------------------------------------------------------------------------
// Launching the installed program as the interactive user
// ---------------------------------------------------------------------------

// The active interactive user's primary token, obtained WITHOUT a window.
//
// `GetShellWindow()` is session-scoped: it returns the shell of the caller's own
// session, which is null when setup was launched by the LocalSystem service and
// is therefore running in session 0.  That is not a corner case -- it is every
// in-app update, because the updater deliberately drives setup from the service
// so the install needs no UAC prompt.
//
// WTSQueryUserToken answers the same question by session id instead of by
// window, and works precisely because setup is running as SYSTEM.  Returns null
// when there is no interactive session (a genuinely headless update), which is
// a legitimate answer and not an error.
static HANDLE gc_active_user_primary_token(DWORD preferredSessionId) {
    DWORD sessionId = preferredSessionId;
    if (sessionId == (DWORD)-1) sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFFu) return nullptr;

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        gc_log_step("launch: WTSQueryUserToken(session %lu) failed (error %lu)",
                    sessionId, GetLastError());
        return nullptr;
    }
    GcScopedHandle scopedUser(userToken);
    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(userToken,
                          TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                              TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                          nullptr, SecurityImpersonation, TokenPrimary,
                          &primaryToken)) {
        gc_log_step("launch: DuplicateTokenEx for session %lu failed (error %lu)",
                    sessionId, GetLastError());
        return nullptr;
    }
    gc_log_step("launch: obtained the interactive token for session %lu", sessionId);
    return primaryToken;
}

bool gc_launch_installed_gui(const WCHAR* installDirectory,
                             DWORD preferredSessionId) {
    WCHAR exePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_join_path(installDirectory, GC_SETUP_GUI_EXE_W, exePath, GC_ARRAY_COUNT(exePath))) return false;
    if (!gc_file_exists(exePath)) return false;

    // Setup runs elevated; starting the GUI directly from here would hand it an
    // administrator token it is not designed to hold (its manifest asks for
    // asInvoker).  Borrowing the desktop shell's token starts it at the
    // interactive user's normal integrity level instead.
    HWND shell = preferredSessionId == (DWORD)-1 ? GetShellWindow() : nullptr;
    if (shell) {
        DWORD shellProcessId = 0;
        GetWindowThreadProcessId(shell, &shellProcessId);
        if (shellProcessId != 0) {
            GcScopedHandle shellProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellProcessId));
            if (shellProcess.valid()) {
                HANDLE shellToken = nullptr;
                if (OpenProcessToken(shellProcess.get(), TOKEN_DUPLICATE, &shellToken)) {
                    GcScopedHandle scopedShellToken(shellToken);
                    HANDLE primaryToken = nullptr;
                    if (DuplicateTokenEx(shellToken,
                                         TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                                             TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                         nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
                        GcScopedHandle scopedPrimary(primaryToken);
                        WCHAR commandLine[GC_INSTALLER_MAX_PATH_CHARS + 8] = {};
                        StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine), L"\"%ls\"", exePath);
                        STARTUPINFOW startup = {};
                        startup.cb = sizeof(startup);
                        PROCESS_INFORMATION process = {};
                        if (CreateProcessWithTokenW(primaryToken, 0, exePath, commandLine, 0, nullptr,
                                                    installDirectory, &startup, &process)) {
                            CloseHandle(process.hProcess);
                            CloseHandle(process.hThread);
                            gc_log_step("launch: started %ls as the interactive user", exePath);
                            return true;
                        }
                        gc_log_step("launch: CreateProcessWithTokenW failed (error %lu)", GetLastError());
                    }
                }
            }
        }
    }

    // No shell window in this session.  That is the normal state when the
    // updater's service launched setup (session 0), so ask WTS for the
    // interactive user's token by session id instead of by window.
    //
    // CreateProcessAsUser rather than CreateProcessWithTokenW: the latter needs
    // SE_IMPERSONATE_NAME and starts the process in the CALLER's session, which
    // from session 0 would produce a GUI nobody can see.  CreateProcessAsUser
    // honours the token's own session, and SYSTEM holds the privileges it wants.
    {
        HANDLE primaryToken = gc_active_user_primary_token(preferredSessionId);
        if (primaryToken) {
            GcScopedHandle scopedPrimary(primaryToken);
            WCHAR commandLine[GC_INSTALLER_MAX_PATH_CHARS + 8] = {};
            StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine), L"\"%ls\"", exePath);
            STARTUPINFOW startup = {};
            startup.cb = sizeof(startup);
            // The interactive desktop, or the process starts with no station to
            // draw on and dies immediately.
            WCHAR desktop[] = L"winsta0\\default";
            startup.lpDesktop = desktop;
            // The user's OWN environment block, not this process's.
            //
            // CreateProcessAsUser with lpEnvironment=nullptr hands the child the
            // CALLER's environment -- and the caller here is a SYSTEM service in
            // session 0.  The child would then see SYSTEM's %USERPROFILE% and
            // %LOCALAPPDATA%, so anything that resolves a per-user path can land
            // in C:\Windows\System32\config\systemprofile instead of the real
            // user's profile.  CreateEnvironmentBlock builds the correct one from
            // the token.
            LPVOID environment = nullptr;
            BOOL haveEnvironment = CreateEnvironmentBlock(&environment, primaryToken, FALSE);
            if (!haveEnvironment) {
                gc_log_step("launch: CreateEnvironmentBlock failed (error %lu); "
                            "refusing to start it with SYSTEM's environment",
                            GetLastError());
                return false;
            }
            PROCESS_INFORMATION process = {};
            BOOL started = CreateProcessAsUserW(
                primaryToken, exePath, commandLine, nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, haveEnvironment ? environment : nullptr,
                installDirectory, &startup, &process);
            DWORD startError = started ? 0 : GetLastError();
            if (haveEnvironment) DestroyEnvironmentBlock(environment);
            if (started) {
                CloseHandle(process.hProcess);
                CloseHandle(process.hThread);
                gc_log_step("launch: started %ls in the interactive session", exePath);
                return true;
            }
            gc_log_step("launch: CreateProcessAsUser failed (error %lu)", startError);
        }
    }

    gc_log_step("launch: could not start %ls as an interactive user "
                "(last error %lu)", exePath, GetLastError());
    return false;
}
