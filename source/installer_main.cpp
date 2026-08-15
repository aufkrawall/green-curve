// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Entry point for the Green Curve setup program and its uninstaller.
//
// Both binaries are built from this file; GREEN_CURVE_UNINSTALLER selects which
// one.  The uninstaller ships inside the setup payload and lands next to the
// program, which is what makes the Add/Remove Programs entry work without the
// multi-megabyte setup file having to stay on disk.
//
// Silent mode (/S) is the path a future in-app updater will drive: no window,
// no prompts, every decision taken from the recorded previous install, and a
// process exit code as the only result.  Everything the interactive run does is
// reached through the same plan and the same executor, so the two cannot drift.

#include "installer_common.h"

// Exit codes.  Documented because an updater will branch on them.
#define GC_EXIT_OK 0
#define GC_EXIT_FAILED 1
#define GC_EXIT_CANCELLED 2
#define GC_EXIT_BAD_ARGUMENTS 3

static const char* const GC_USAGE_TEXT =
    "Green Curve " APP_VERSION " setup\n"
    "\n"
    "  /S, --silent           Install or update without showing a window.\n"
    "  /D=<path>, --dir <p>   Install into <path> (the \"Green Curve\" folder itself).\n"
    "  --start-menu           Create a Start menu shortcut (default on).\n"
    "  --no-start-menu        Do not create a Start menu shortcut.\n"
    "  --desktop              Create a desktop shortcut.\n"
    "  --no-desktop           Do not create a desktop shortcut (default).\n"
    "  --launch               Start Green Curve when setup finishes.\n"
    "  --no-launch            Do not start Green Curve afterwards (default when silent).\n"
    "  --uninstall            Remove an existing installation.\n"
    "  /log=<name>            Write a new failure log named <name> next to setup.\n"
    "  /?, --help             Show this text.\n"
    "\n"
    "Exit codes: 0 success, 1 failure, 2 cancelled, 3 bad arguments.\n"
    "A log file is written next to setup only when something failed.";

static bool gc_process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
                    elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

// Convert the wide command line into the UTF-8 argv the pure parser expects.
// Doing the conversion here keeps every parsing rule in a header that
// `build.py --test` can exercise without a Windows process.
struct GcArgumentVector {
    char storage[16][GC_INSTALLER_MAX_PATH_CHARS];
    const char* pointers[16];
    int count;
};

static bool gc_build_argument_vector(GcArgumentVector* vector, char* error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    vector->count = 0;
    int wideCount = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (!wideArgv) return true;  // no arguments at all
    bool ok = true;
    for (int i = 1; i < wideCount; i++) {
        if (vector->count >= (int)GC_ARRAY_COUNT(vector->pointers)) {
            if (error && errorSize) {
                StringCchCopyA(error, errorSize, "Too many command line arguments.");
            }
            ok = false;
            break;
        }
        char* slot = vector->storage[vector->count];
        if (!gc_wide_to_utf8(wideArgv[i], slot, GC_INSTALLER_MAX_PATH_CHARS)) {
            if (error && errorSize) {
                StringCchCopyA(error, errorSize, "A command line argument could not be decoded.");
            }
            ok = false;
            break;
        }
        vector->pointers[vector->count] = slot;
        vector->count++;
    }
    LocalFree(wideArgv);
    return ok;
}

// ---------------------------------------------------------------------------
// Silent execution
// ---------------------------------------------------------------------------

static int gc_run_silent_install(const GcInstallerOptions* options, const GcPriorInstall* prior,
                                 const char* defaultDirectory) {
    GcInstallContext context = {};
    gc_install_build_plan(options, prior, defaultDirectory, &context.plan);
    if (!context.plan.valid) {
        gc_log_fail("silent: %s", context.plan.error);
        return GC_EXIT_BAD_ARGUMENTS;
    }
    WCHAR modulePath[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_module_path(modulePath, GC_ARRAY_COUNT(modulePath)) ||
        !gc_payload_load(modulePath, &context.payload)) {
        gc_log_fail("silent: this setup file carries no program files");
        return GC_EXIT_FAILED;
    }
    bool ok = gc_install_execute(&context);
    // Relaunch even when the install FAILED, which is why this is no longer
    // gated on `ok`.
    //
    // The updater closes every Green Curve window before starting setup --
    // setup runs in session 0 and cannot reach across to them itself -- so a
    // failed silent install used to leave the user with no window, no tray
    // icon, and nothing on screen to explain why the program had vanished.
    // Reporting the failure into a GUI that has been closed reports it to
    // nobody.
    //
    // Safe because gc_launch_installed_gui() refuses when greencurve.exe is not
    // present, so a half-copied installation starts nothing at all rather than
    // something broken; and because a GUI that no longer matches its service is
    // a case the protocol version check already handles visibly.
    if (context.plan.launchAfterInstall) {
        WCHAR directory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (gc_utf8_to_wide(context.plan.targetDirectory, directory, (int)GC_ARRAY_COUNT(directory))) {
            gc_launch_installed_gui(
                directory,
                options->hasLaunchSession ? options->launchSessionId : (DWORD)-1);
        }
    }
    gc_payload_release(&context.payload);
    return ok ? GC_EXIT_OK : GC_EXIT_FAILED;
}

static int gc_run_silent_uninstall(const WCHAR* installDirectory) {
    char error[512] = {};
    bool ok = gc_uninstall_execute(installDirectory, error, sizeof(error));
    if (!ok) gc_log_fail("silent uninstall: %s", error[0] ? error : "unknown failure");
    return ok ? GC_EXIT_OK : GC_EXIT_FAILED;
}

// ---------------------------------------------------------------------------
// Where an uninstall should point
// ---------------------------------------------------------------------------

// The uninstaller lives inside the installation, so its own directory is the
// answer.  The setup binary run with --uninstall has to ask the registry
// instead, because it may be sitting in a downloads folder.
static bool gc_resolve_uninstall_directory(WCHAR* out, size_t outCount) {
#if defined(GREEN_CURVE_UNINSTALLER)
    if (gc_module_directory(out, outCount)) return true;
#endif
    GcPriorInstall prior = {};
    if (gc_read_prior_install(&prior) && prior.directory[0]) {
        return gc_utf8_to_wide(prior.directory, out, (int)outCount);
    }
#if !defined(GREEN_CURVE_UNINSTALLER)
    return false;
#else
    return gc_module_directory(out, outCount);
#endif
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// WinMain rather than wWinMain: the MinGW CRT looks for the narrow entry point
// unless the link adds -municode, and the narrow parameters are unused anyway —
// the real command line is read wide via GetCommandLineW().
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    // Per-monitor-v2 is requested by the manifest; this call also covers
    // launchers that strip or override manifest awareness, which is exactly the
    // case an in-app updater creates when it spawns setup from its own process.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFn)(HANDLE);
        auto setContext =
            (SetProcessDpiAwarenessContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        // -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, spelled as its
        // documented pseudo-handle so an older import library still builds.
        if (!setContext || !setContext((HANDLE)(INT_PTR)-4)) SetProcessDPIAware();
    }

    GcArgumentVector arguments = {};
    char argumentError[192] = {};
    bool argumentsOk = gc_build_argument_vector(&arguments, argumentError, sizeof(argumentError));

    GcInstallerOptions options = {};
    gc_installer_parse_options(arguments.count, arguments.pointers, &options);
#if defined(GREEN_CURVE_UNINSTALLER)
    // The uninstaller has exactly one job; --uninstall is implied.
    if (options.valid && options.mode == GC_INSTALLER_MODE_INSTALL) {
        options.mode = GC_INSTALLER_MODE_UNINSTALL;
    }
#endif

    WCHAR logOverride[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (options.valid && options.hasLogPath) {
        gc_utf8_to_wide(options.logPath, logOverride, (int)GC_ARRAY_COUNT(logOverride));
    }
    gc_log_init(logOverride[0] ? logOverride : nullptr);

    if (!argumentsOk) {
        gc_log_fail("arguments: %s", argumentError);
        gc_show_message(nullptr, argumentError, "Green Curve Setup", true);
        gc_log_flush_on_failure();
        return GC_EXIT_BAD_ARGUMENTS;
    }
    if (!options.valid) {
        gc_log_fail("arguments: %s", options.error);
        char message[512] = {};
        snprintf(message, sizeof(message), "%s\n\n%s", options.error, GC_USAGE_TEXT);
        gc_show_message(nullptr, message, "Green Curve Setup", true);
        gc_log_flush_on_failure();
        return GC_EXIT_BAD_ARGUMENTS;
    }
    if (options.mode == GC_INSTALLER_MODE_HELP) {
        gc_show_message(nullptr, GC_USAGE_TEXT, "Green Curve Setup", false);
        return GC_EXIT_OK;
    }

    // Registering a Windows service and writing under Program Files both need
    // administrator rights.  The manifest requests them, so reaching this check
    // unelevated means the manifest was stripped or bypassed; saying so beats
    // failing later with an opaque access-denied.
    if (!gc_process_is_elevated()) {
        const char* message =
            "Setup needs administrator rights to register the Green Curve background service. "
            "Right-click setup and choose \"Run as administrator\".";
        gc_log_fail("elevation: the setup process is not elevated");
        if (!options.silent) gc_show_message(nullptr, message, "Green Curve Setup", true);
        gc_log_flush_on_failure();
        return GC_EXIT_FAILED;
    }

    HRESULT comStatus = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comReady = SUCCEEDED(comStatus);
    if (!comReady) gc_log_step("COM could not be initialized (hr 0x%08lx); shortcuts may be skipped", comStatus);

    int exitCode = GC_EXIT_FAILED;
    if (options.mode == GC_INSTALLER_MODE_UNINSTALL) {
        WCHAR installDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_resolve_uninstall_directory(installDirectory, GC_ARRAY_COUNT(installDirectory))) {
            const char* message = "No Green Curve installation was found to remove.";
            gc_log_fail("uninstall: %s", message);
            if (!options.silent) gc_show_message(nullptr, message, "Green Curve", true);
            exitCode = GC_EXIT_FAILED;
        } else if (options.silent) {
            exitCode = gc_run_silent_uninstall(installDirectory);
        } else {
            exitCode = gc_run_uninstall_window(instance, installDirectory);
        }
    } else {
        GcPriorInstall prior = {};
        gc_read_prior_install(&prior);
        char defaultDirectory[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (!gc_default_install_directory(defaultDirectory, sizeof(defaultDirectory))) {
            // Only reachable if Program Files itself cannot be resolved; a
            // hard-coded fallback still lets the user pick something sane.
            StringCchCopyA(defaultDirectory, GC_ARRAY_COUNT(defaultDirectory),
                           "C:\\Program Files\\" GC_INSTALL_FOLDER_NAME);
            gc_log_step("default directory: Program Files could not be resolved; using %s", defaultDirectory);
        }
        if (options.silent) {
            exitCode = gc_run_silent_install(&options, &prior, defaultDirectory);
        } else {
            exitCode = gc_run_setup_wizard(instance, &options, &prior, defaultDirectory);
        }
    }

    if (comReady) CoUninitialize();

    const WCHAR* logPath = gc_log_flush_on_failure();
    if (logPath && !options.silent && exitCode == GC_EXIT_FAILED) {
        char message[768] = {};
        char logPathUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
        gc_wide_to_utf8(logPath, logPathUtf8, (int)sizeof(logPathUtf8));
        snprintf(message, sizeof(message),
                 "Setup did not finish. A log describing what failed was written to:\n\n%s", logPathUtf8);
        gc_show_message(nullptr, message, "Green Curve Setup", true);
    }
    return exitCode;
}
