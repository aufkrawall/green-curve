// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the real GUI notice code with a tray/status sink, without a window
// or service. Assertion codes 6840-6859.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "gpu_core.h"
#include "auto_apply_failure_notice_policy.h"

#ifndef ARRAY_COUNT
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

namespace gui_notice_fixture {

struct NOTIFYICONDATAA {
    unsigned int cbSize;
    void* hWnd;
    unsigned int uID, uFlags, dwInfoFlags;
    char szInfoTitle[64], szInfo[256];
};
enum { NIF_INFO = 1, NIIF_WARNING = 2, NIIF_NOSOUND = 4, NIM_MODIFY = 1 };
static struct {
    bool trayIconAdded;
    void* hMainWnd;
    ServiceAutoRestoreLockoutReason serviceAutoRestoreLockoutReason;
    ServiceAutoApplyFailure serviceAutoApplyFailure;
} g_app = {};
static char status[256] = {};
static int statusWrites = 0, balloonAttempts = 0, balloonsShown = 0;
static bool shellAccepts = true;

static void StringCchCopyA(char* out, size_t size, const char* value) {
    snprintf(out, size, "%s", value);
}
static unsigned long GetLastError() { return 0; }
static void debug_log(const char*, ...) {}
static void debug_log_on_change(const char*, ...) {}
static void set_profile_status_text(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    statusWrites++;
}
static bool Shell_NotifyIconA(unsigned int, const NOTIFYICONDATAA* info) {
    balloonAttempts++;
    if (info->uFlags != NIF_INFO || !(info->dwInfoFlags & NIIF_NOSOUND) ||
        !info->szInfoTitle[0] || !info->szInfo[0]) return false;
    if (shellAccepts) balloonsShown++;
    return shellAccepts;
}

#include "gui_auto_restore_notice.h"

static void announce() {
    gui_announce_auto_restore_lockout();
}

static int run() {
    const auto NONE = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    const auto FAILED = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    const auto HANDBACK = SERVICE_AUTO_RESTORE_LOCKOUT_HANDBACK_INCOMPLETE;
    // The pre-window snapshot already adopted this failure. The first UI
    // adoption still owes a notice, after its generic/startup status writes.
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    g_app.serviceAutoApplyFailure.trigger = SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON;
    g_app.serviceAutoApplyFailure.coreRecovered = 1;
    snprintf(g_app.serviceAutoApplyFailure.detail,
        sizeof(g_app.serviceAutoApplyFailure.detail), "fixture cause");
    g_app.trayIconAdded = true;
    set_profile_status_text("Live GPU state refreshed.");
    set_profile_status_text("Showing current GPU state.");
    announce();
    if (!strstr(status, "at logon failed") || !strstr(status, "fixture cause") ||
        !strstr(status, "Press Apply") || balloonsShown != 1) return 6840;
    int writes = statusWrites;
    announce();
    if (statusWrites != writes || balloonsShown != 1) return 6841;

    g_app.serviceAutoRestoreLockoutReason = NONE;
    announce();
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    g_app.trayIconAdded = false;
    announce();
    if (!g_guiAutoApplyBalloonPending || balloonsShown != 1) return 6842;
    g_app.trayIconAdded = true;
    shellAccepts = false;
    announce();
    if (!g_guiAutoApplyBalloonPending || balloonsShown != 1) return 6843;
    shellAccepts = true;
    announce();
    if (g_guiAutoApplyBalloonPending || balloonsShown != 2) return 6844;
    int attempts = balloonAttempts;
    announce();
    if (balloonAttempts != attempts) return 6845;

    g_app.serviceAutoRestoreLockoutReason = HANDBACK;
    announce();
    if (!strstr(status, "Press Reset")) return 6846;
    writes = statusWrites;
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    announce();
    if (statusWrites != writes || balloonsShown != 2) return 6847;

    g_app.serviceAutoRestoreLockoutReason = NONE;
    announce();
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    g_app.serviceAutoApplyFailure = ServiceAutoApplyFailure{};
    announce();
    if (!strstr(status, "paused for safety") || balloonsShown != 3 ||
        strstr(g_guiAutoApplyBalloonTitle, "automatic apply failed") != nullptr ||
        strstr(g_guiAutoApplyBalloonBody, "automatic apply failed") != nullptr)
        return 6848;
    g_app.serviceAutoRestoreLockoutReason = NONE;
    announce();
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    announce();
    if (balloonsShown != 3) return 6849;

    g_app.serviceAutoRestoreLockoutReason = NONE;
    announce();
    g_app.serviceAutoRestoreLockoutReason = FAILED;
    g_app.serviceAutoApplyFailure.trigger = SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
    g_app.trayIconAdded = false;
    announce();
    if (!g_guiAutoApplyBalloonPending) return 6850;
    g_app.serviceAutoRestoreLockoutReason = NONE;
    announce();
    g_app.trayIconAdded = true;
    announce();
    if (g_guiAutoApplyBalloonPending || balloonsShown != 3) return 6851;
    return 0;
}
} // namespace gui_notice_fixture

int run_gui_auto_restore_notice_tests() { return gui_notice_fixture::run(); }

#ifdef GC_GUI_NOTICE_FIXTURE_MAIN
int main() {
    int result = run_gui_auto_restore_notice_tests();
    if (result) fprintf(stderr, "GUI notice assertion failed: %d\n", result);
    return result ? 1 : 0;
}
#endif
