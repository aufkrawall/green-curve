// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shared declarations for the standalone Green Curve setup program.
//
// The installer is deliberately independent of the application's own headers:
// app_shared.h pulls in the GPU model, the service protocol, and the whole GUI
// state machine, none of which a setup program should be able to touch.  The
// one thing it does share is the window palette (theme_palette.h), because the
// setup window has to look like the program it installs.
//
// Two binaries are built from these sources:
//   * the setup stub, which carries the payload (GREEN_CURVE_UNINSTALLER unset)
//   * uninstall.exe, which ships inside that payload (GREEN_CURVE_UNINSTALLER=1)
// Both share the window, the theme, and the service/shortcut handling; only the
// work they perform differs.

#ifndef GREEN_CURVE_INSTALLER_COMMON_H
#define GREEN_CURVE_INSTALLER_COMMON_H

// The setup program is wide-character throughout; defining UNICODE makes the
// resource pseudo-macros (IDC_ARROW, IDI_APPLICATION) expand to their wide form
// so they can be passed to the explicitly-W entry points used everywhere here.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
// WTSQueryUserToken / WTSGetActiveConsoleSessionId, used to reach the
// interactive session when setup runs in session 0 (the in-app updater
// launches it from the LocalSystem service).
#include <wtsapi32.h>
#include <shlobj.h>
#include <objbase.h>
#include <uxtheme.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "theme_palette.h"
#include "installer_archive_policy.h"
#include "installer_cli_policy.h"
#include "installer_plan_policy.h"
#include "installer_uninstall_policy.h"
#include "service_acl.h"

#ifndef APP_VERSION
// build.py injects the real version.  The neutral fallback matches the
// application's convention so a stray uninjected build is obvious rather than
// silently claiming to be a release.
#define APP_VERSION "dev"
#endif

#define GC_SETUP_PRODUCT_NAME "Green Curve"
#define GC_SETUP_PRODUCT_NAME_W L"Green Curve"
#define GC_SETUP_PUBLISHER "aufkrawall"
#define GC_SETUP_PUBLISHER_W L"aufkrawall"
#define GC_SETUP_GUI_EXE "greencurve.exe"
#define GC_SETUP_GUI_EXE_W L"greencurve.exe"
#define GC_SETUP_SERVICE_EXE "greencurve-service.exe"
#define GC_SETUP_SERVICE_EXE_W L"greencurve-service.exe"
#define GC_SETUP_UNINSTALL_EXE "uninstall.exe"
#define GC_SETUP_UNINSTALL_EXE_W L"uninstall.exe"
#define GC_SETUP_SERVICE_NAME L"GreenCurveService"
#define GC_SETUP_WINDOW_CLASS L"GreenCurveSetupClass"
// Resource id of the Green Curve icon embedded in both setup binaries.  It must
// stay equal to the id emitted by INSTALLER_RC in tools/installer_build.py (and
// to APP_ICON_ID, which the application's own icon.rc uses) -- the shell picks
// the *lowest-numbered* icon for the file, but a window class has to name one.
// Without this the title bar, Alt-Tab and the taskbar all showed the stock
// Windows application icon while the file itself showed the right one.
#define GC_SETUP_ICON_ID 101
// Wide spelling of the per-user Run value the resident tray GUI is started
// from.  The narrow one lives in installer_uninstall_policy.h next to the rule
// that decides whether a given Run value is ours to delete.
#define GC_TRAY_AUTOSTART_VALUE_NAME_W L"Green Curve"
// The application's own main-window class.  Closing these windows is how the
// running GUI is asked to shut down cleanly before its files are replaced.
#define GC_APP_WINDOW_CLASS L"GreenCurveClass"

// Machine-wide record of the installation.  Doubles as the Add/Remove Programs
// entry, so "where is Green Curve installed" has exactly one answer.
#define GC_SETUP_UNINSTALL_KEY \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Green Curve"

// Capability marker: the greencurve.exe this installer places understands
// --export-active-settings, so the next upgrade may ask it for the live
// settings instead of guessing from a version number.  Written as a value
// rather than inferred, because the binary in the install directory is the only
// thing that actually answers the question.
#define GC_SETUP_SETTINGS_EXPORT_VALUE L"GreenCurveSettingsExport"

#define GC_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------
// Failure log
//
// Written next to the running setup executable, and ONLY when something failed:
// a successful install leaves no litter behind.  Every step reports through
// gc_log_step()/gc_log_fail() so a support request can be answered from one
// file that shows how far the install got.
// ---------------------------------------------------------------------------
void gc_log_init(const WCHAR* overridePath);
// The format attributes matter: -Werror plus -Wformat=2 then checks every call
// site here the same way it checks the application's own debug_log().
[[gnu::format(printf, 1, 2)]] void gc_log_step(const char* fmt, ...);
[[gnu::format(printf, 1, 2)]] void gc_log_fail(const char* fmt, ...);
bool gc_log_had_failure();
// Flush the buffered transcript to disk if (and only if) a failure was
// recorded.  Returns the path written, or nullptr when nothing was written.
const WCHAR* gc_log_flush_on_failure();

// ---------------------------------------------------------------------------
// Small Win32 helpers shared by every installer shard
// ---------------------------------------------------------------------------

struct GcScopedHandle {
    HANDLE handle;
    GcScopedHandle() : handle(nullptr) {}
    explicit GcScopedHandle(HANDLE value) : handle(value) {}
    ~GcScopedHandle() { reset(); }
    GcScopedHandle(const GcScopedHandle&) = delete;
    GcScopedHandle& operator=(const GcScopedHandle&) = delete;
    HANDLE get() const { return handle; }
    bool valid() const { return handle && handle != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (valid()) CloseHandle(handle);
        handle = value;
    }
};

struct GcScopedServiceHandle {
    SC_HANDLE handle;
    GcScopedServiceHandle() : handle(nullptr) {}
    explicit GcScopedServiceHandle(SC_HANDLE value) : handle(value) {}
    ~GcScopedServiceHandle() { reset(); }
    GcScopedServiceHandle(const GcScopedServiceHandle&) = delete;
    GcScopedServiceHandle& operator=(const GcScopedServiceHandle&) = delete;
    SC_HANDLE get() const { return handle; }
    bool valid() const { return handle != nullptr; }
    void reset(SC_HANDLE value = nullptr) {
        if (handle) CloseServiceHandle(handle);
        handle = value;
    }
};

bool gc_utf8_to_wide(const char* utf8, WCHAR* out, int outCount);
bool gc_wide_to_utf8(const WCHAR* wide, char* out, int outCount);
// Full path of the running executable, and the directory that contains it.
bool gc_module_path(WCHAR* out, size_t outCount);
bool gc_module_directory(WCHAR* out, size_t outCount);
bool gc_join_path(const WCHAR* directory, const WCHAR* leaf, WCHAR* out, size_t outCount);
bool gc_directory_of(const WCHAR* filePath, WCHAR* out, size_t outCount);
// Create `path` and every missing parent.  Returns false only if the directory
// does not exist afterwards.
bool gc_create_directory_tree(const WCHAR* path);
bool gc_file_exists(const WCHAR* path);
bool gc_directory_exists(const WCHAR* path);
// Create a cryptographically unpredictable, administrator-only directory
// directly beneath Program Files. The secure parent plus an atomically applied
// protected DACL prevents an unelevated process from replacing a staged
// executable between extraction and CreateProcess.
bool gc_create_private_temp_directory(WCHAR* out, size_t outCount);
// Installer service binaries run as LocalSystem. Accept installation only
// as a direct Program Files child, where an unprivileged intermediate parent
// cannot substitute the protected directory after registration.
bool gc_install_directory_is_secure_rooted(const WCHAR* path);
// Strip a quoted/unquoted argv[0] out of an SCM ImagePath and return its
// directory, which is where a previously registered service lives.
bool gc_service_image_directory(WCHAR* out, size_t outCount);
// Run a program and wait for it, bounded.  `exitCodeOut` receives its exit
// code.  Used for the application's own --service-install and settings CLI.
bool gc_run_and_wait(const WCHAR* exePath, const WCHAR* commandLine, DWORD timeoutMs,
                     DWORD* exitCodeOut);
// Delete a file that is the image of the running process, immediately.
//
// Windows refuses an ordinary delete on a mapped image, which is why the
// uninstaller used to hand itself to the session manager and leave
// uninstall.exe — and therefore the whole install folder — sitting there until
// the next restart.  Returns false when the file is still present afterwards,
// so the caller can fall back to MOVEFILE_DELAY_UNTIL_REBOOT.
bool gc_delete_running_module(const WCHAR* path);

// ---------------------------------------------------------------------------
// Autostart removal (source/installer_autostart.cpp)
//
// Neither registration lives in the install directory or under the uninstall
// key, so both outlive a file deletion unless they are removed explicitly.
// Neither can fail an uninstall; failures are recorded in the transcript.
// ---------------------------------------------------------------------------

// Every `Green Curve Startup - <user>` logon task on the machine.
void gc_remove_startup_tasks();
// The resident-tray Run value in every user hive that is currently loaded.
void gc_remove_tray_autostart_values();

// ---------------------------------------------------------------------------
// Payload (source/installer_payload.cpp)
// ---------------------------------------------------------------------------

struct GcPayloadFile {
    char name[GC_ARCHIVE_MAX_NAME + 1];
    const uint8_t* data;
    uint64_t size;
    uint32_t flags;
};

struct GcPayload {
    // Owned decompressed container.  Freed by gc_payload_release().
    uint8_t* container;
    uint64_t containerSize;
    GcPayloadFile files[GC_ARCHIVE_MAX_FILES];
    uint32_t fileCount;
};

// Read the payload appended to `exePath` (normally the running setup binary).
// On failure the reason is logged and false is returned.
bool gc_payload_load(const WCHAR* exePath, GcPayload* payload);
void gc_payload_release(GcPayload* payload);
const GcPayloadFile* gc_payload_find(const GcPayload* payload, const char* name);

// ---------------------------------------------------------------------------
// Install / uninstall actions (source/installer_apply.cpp)
// ---------------------------------------------------------------------------

// Reported back to the UI so the progress page can name the current step.
typedef void (*GcProgressFn)(void* context, int percent, const char* status);

struct GcInstallContext {
    GcInstallPlan plan;
    GcPayload payload;
    GcProgressFn progress;
    void* progressContext;
    // Where the pre-upgrade settings snapshot was written, if one was taken.
    // Sized like every other installer path buffer: the snapshot path is built
    // in GC_INSTALLER_MAX_PATH_CHARS storage, and a narrower field here would
    // truncate it silently into a restore that cannot find its own file.
    WCHAR capturedSettingsPath[GC_INSTALLER_MAX_PATH_CHARS];
    bool haveCapturedSettings;
    // Outcome of the restore, reported on the final page.  A failed restore does
    // not fail the installation, but silently leaving the GPU at stock after an
    // upgrade is exactly the kind of thing a user should be told about.
    bool settingsRestoreAttempted;
    bool settingsRestored;
    // Filled with an actionable message when a step fails.
    char error[512];
};

bool gc_read_prior_install(GcPriorInstall* prior);
bool gc_default_install_directory(char* out, size_t outCount);
bool gc_install_execute(GcInstallContext* context);
bool gc_uninstall_execute(const WCHAR* installDirectory, char* error, size_t errorSize);
// Ask every running Green Curve GUI to close and wait for the processes to go
// away.  `context` may be null (the uninstaller has no progress reporting).
bool gc_stop_gui_processes(GcInstallContext* context);
// Shortcuts plus the Add/Remove Programs record (source/installer_register.cpp).
bool gc_write_shortcuts_and_registration(GcInstallContext* context);
// Launch the installed GUI as the interactive (unelevated) user.
bool gc_launch_installed_gui(const WCHAR* installDirectory);

// ---------------------------------------------------------------------------
// UI (source/installer_ui.cpp)
// ---------------------------------------------------------------------------

// True when Windows is set to dark for apps.  Drives the title bar only: the
// client area always uses the Green Curve palette, exactly like the program's
// own main window.
bool gc_system_dark_theme_active();
void gc_apply_titlebar_theme(HWND hwnd);

// Run the interactive setup wizard.  Returns the process exit code.
int gc_run_setup_wizard(HINSTANCE instance, const GcInstallerOptions* options,
                        const GcPriorInstall* prior, const char* defaultDirectory);
// Run the interactive uninstall confirmation.  Returns the process exit code.
int gc_run_uninstall_window(HINSTANCE instance, const WCHAR* installDirectory);
// Themed, DPI-correct replacement for MessageBox, used for silent-mode and
// early failures that have no wizard window yet.
void gc_show_message(HWND owner, const char* text, const char* caption, bool error);

#endif // GREEN_CURVE_INSTALLER_COMMON_H
