// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Removing the per-user autostart registrations at uninstall time.
//
// Green Curve starts itself two ways, and neither of them lives in the install
// directory or under the machine-wide uninstall key, so deleting the files and
// the Add/Remove Programs entry used to leave both behind — a logon task that
// pointed at a binary that no longer existed, and a Run value that did the
// same:
//
//   * `Green Curve Startup - <user>` in Task Scheduler, registered by the GUI
//     when a logon profile is chosen (source/main_startup_task_runtime.cpp);
//   * a per-user `HKCU\...\Run` value that launches the resident tray GUI
//     (source/main_tray_autostart.cpp).
//
// Both are registered *by the user who enabled them*, so a machine can carry
// one of each per account.  The uninstaller is elevated, which is what makes it
// the only component able to remove all of them rather than only the ones
// belonging to whoever clicked through the UAC prompt.
//
// Nothing here can fail the uninstall: an autostart entry that survives is a
// leftover, not a broken removal, and refusing to delete the program over it
// would be worse.  Every failure is recorded in the transcript instead, which
// is the file a support report is answered from.

#include "installer_common.h"

// initguid.h re-defines DEFINE_GUID to *emit* each constant instead of merely
// declaring it, so CLSID_TaskScheduler and IID_ITaskService come out of this
// translation unit.  The alternative, -ltaskschd, is not one: mingw ships those
// symbols in a static UUID archive rather than an import library, and Zig's
// arm64 link step rejects the name as a missing dynamic system library.  It
// must come after installer_common.h (which pulls in windows.h) and before the
// header whose GUIDs are wanted.
#include <initguid.h>
#include <taskschd.h>

// Bounded like every other helper setup runs: a wedged Task Scheduler must not
// hang an unattended silent uninstall.
#define GC_SCHTASKS_TIMEOUT_MS 15000
// One task per account that ever enabled a logon profile.  Overflow is logged
// rather than silently dropped, so "we stopped looking" can never read as
// "there was nothing left".
#define GC_MAX_STARTUP_TASKS 64
#define GC_MAX_TASK_NAME_CHARS 256

// ---------------------------------------------------------------------------
// Scheduled logon tasks
// ---------------------------------------------------------------------------

struct GcTaskNameList {
    WCHAR names[GC_MAX_STARTUP_TASKS][GC_MAX_TASK_NAME_CHARS];
    int count;
    bool overflowed;
};

// Accept a candidate only after it round-trips through the pure predicate, so
// the "is this ours?" rule is the one `build.py --test` pins and not a second
// copy written inline against a COM string.
static void gc_task_list_add(GcTaskNameList* list, const WCHAR* name) {
    if (!list || !name || !name[0]) return;
    char utf8[GC_MAX_TASK_NAME_CHARS * 4] = {};
    if (!gc_wide_to_utf8(name, utf8, (int)sizeof(utf8))) {
        gc_log_step("autostart: a scheduled task name could not be decoded; leaving it alone");
        return;
    }
    if (!gc_uninstall_task_name_is_ours(utf8)) return;
    for (int i = 0; i < list->count; i++) {
        if (lstrcmpiW(list->names[i], name) == 0) return;
    }
    if (list->count >= GC_MAX_STARTUP_TASKS) {
        list->overflowed = true;
        return;
    }
    if (FAILED(StringCchCopyW(list->names[list->count], GC_MAX_TASK_NAME_CHARS, name))) return;
    list->count++;
}

// Enumerate the root task folder through the documented API.  Green Curve never
// registers into a subfolder, so the root folder is the whole search space.
static bool gc_collect_startup_tasks_com(ITaskFolder* root, GcTaskNameList* list) {
    if (!root || !list) return false;
    IRegisteredTaskCollection* tasks = nullptr;
    HRESULT hr = root->GetTasks(TASK_ENUM_HIDDEN, &tasks);
    if (FAILED(hr) || !tasks) {
        gc_log_step("autostart: the registered task list could not be read (hr 0x%08lx)",
                    (unsigned long)hr);
        return false;
    }
    LONG count = 0;
    hr = tasks->get_Count(&count);
    if (FAILED(hr)) {
        gc_log_step("autostart: the registered task count could not be read (hr 0x%08lx)",
                    (unsigned long)hr);
        tasks->Release();
        return false;
    }
    for (LONG index = 1; index <= count; index++) {
        VARIANT position;
        VariantInit(&position);
        position.vt = VT_I4;
        position.lVal = index;
        IRegisteredTask* task = nullptr;
        if (FAILED(tasks->get_Item(position, &task)) || !task) {
            VariantClear(&position);
            continue;
        }
        VariantClear(&position);
        BSTR name = nullptr;
        if (SUCCEEDED(task->get_Name(&name)) && name) {
            gc_task_list_add(list, name);
        }
        if (name) SysFreeString(name);
        task->Release();
    }
    tasks->Release();
    return true;
}

// Fallback enumeration for the case the COM path is unavailable at all (a
// failed apartment, a scheduler that will not hand out its collection).
//
// Root-folder tasks are stored one file per task under %SystemRoot%\System32\
// Tasks, named exactly as the task.  That layout is an implementation detail,
// which is why it is only ever used to *find* names — the deletion still goes
// through schtasks.exe, because removing the file alone would strand the
// scheduler's own registry bookkeeping.
static bool gc_collect_startup_tasks_from_store(GcTaskNameList* list) {
    if (!list) return false;
    WCHAR system[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(system, GC_ARRAY_COUNT(system));
    if (length == 0 || length >= GC_ARRAY_COUNT(system)) return false;
    WCHAR pattern[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (FAILED(StringCchPrintfW(pattern, GC_ARRAY_COUNT(pattern), L"%ls\\Tasks\\%hs*",
                                system, GC_STARTUP_TASK_PREFIX))) return false;
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        // No matches is a successful enumeration that found nothing.
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        gc_task_list_add(list, found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return true;
}

static bool gc_enumerate_startup_tasks(ITaskFolder* root, GcTaskNameList* list) {
    if (root && gc_collect_startup_tasks_com(root, list)) return true;
    gc_log_step("autostart: reading the on-disk task store instead of the scheduler API");
    return gc_collect_startup_tasks_from_store(list);
}

// schtasks.exe is the fallback deletion path.  The name is passed inside quotes
// on a command line, so a name that contains a quote is refused rather than
// escaped — the scheduler does not allow one in a task name, and building a
// command line out of a string that might is not worth the risk.
static bool gc_delete_startup_task_via_schtasks(const WCHAR* name) {
    if (!name || !name[0] || wcschr(name, L'"')) return false;
    WCHAR schtasks[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(schtasks, GC_ARRAY_COUNT(schtasks));
    if (length == 0 || length >= GC_ARRAY_COUNT(schtasks)) return false;
    if (FAILED(StringCchCatW(schtasks, GC_ARRAY_COUNT(schtasks), L"\\schtasks.exe"))) return false;
    WCHAR commandLine[1024] = {};
    if (FAILED(StringCchPrintfW(commandLine, GC_ARRAY_COUNT(commandLine),
                                L"\"%ls\" /delete /tn \"%ls\" /f", schtasks, name))) return false;
    DWORD exitCode = (DWORD)-1;
    if (!gc_run_and_wait(schtasks, commandLine, GC_SCHTASKS_TIMEOUT_MS, &exitCode)) return false;
    // schtasks reports a non-zero code for "already gone" as well as for
    // "refused", so its exit code is not the answer; the caller re-enumerates.
    gc_log_step("autostart: schtasks /delete \"%ls\" exited %lu", name, exitCode);
    return exitCode == 0;
}

static void gc_delete_startup_task(ITaskFolder* root, const WCHAR* name) {
    if (!name || !name[0]) return;
    if (root) {
        BSTR taskName = SysAllocString(name);
        if (taskName) {
            HRESULT hr = root->DeleteTask(taskName, 0);
            SysFreeString(taskName);
            if (SUCCEEDED(hr)) {
                gc_log_step("autostart: removed the logon task \"%ls\"", name);
                return;
            }
            gc_log_step("autostart: the scheduler refused to delete \"%ls\" (hr 0x%08lx); trying schtasks",
                        name, (unsigned long)hr);
        }
    }
    if (gc_delete_startup_task_via_schtasks(name)) {
        gc_log_step("autostart: removed the logon task \"%ls\" via schtasks", name);
    }
}

// Remove every `Green Curve Startup - <user>` logon task on the machine.
void gc_remove_startup_tasks() {
    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, (void**)&service);
    if (SUCCEEDED(hr) && service) {
        VARIANT empty;
        VariantInit(&empty);
        hr = service->Connect(empty, empty, empty, empty);
        if (SUCCEEDED(hr)) {
            BSTR rootPath = SysAllocString(L"\\");
            hr = rootPath ? service->GetFolder(rootPath, &root) : E_OUTOFMEMORY;
            if (rootPath) SysFreeString(rootPath);
        }
        if (FAILED(hr)) {
            gc_log_step("autostart: the Task Scheduler service could not be reached (hr 0x%08lx)",
                        (unsigned long)hr);
        }
    } else {
        gc_log_step("autostart: the Task Scheduler object could not be created (hr 0x%08lx)",
                    (unsigned long)hr);
    }

    GcTaskNameList found = {};
    bool enumerated = gc_enumerate_startup_tasks(root, &found);
    if (!enumerated) {
        gc_log_fail("autostart: the scheduled logon tasks could not be enumerated; "
                    "any that exist were left registered");
    } else {
        if (found.overflowed) {
            gc_log_fail("autostart: more than %d Green Curve logon tasks are registered; "
                        "only the first %d were removed",
                        GC_MAX_STARTUP_TASKS, GC_MAX_STARTUP_TASKS);
        }
        if (found.count == 0) {
            gc_log_step("autostart: no Green Curve logon task is registered");
        }
        for (int i = 0; i < found.count; i++) gc_delete_startup_task(root, found.names[i]);

        // Verify by state rather than by return codes: both deletion paths
        // report failure for "already absent" as readily as for "refused".
        GcTaskNameList remaining = {};
        if (found.count > 0 && gc_enumerate_startup_tasks(root, &remaining)) {
            for (int i = 0; i < remaining.count; i++) {
                gc_log_fail("autostart: the logon task \"%ls\" is still registered after removal",
                            remaining.names[i]);
            }
        }
    }

    if (root) root->Release();
    if (service) service->Release();
}

// ---------------------------------------------------------------------------
// Per-user tray Run values
// ---------------------------------------------------------------------------

// Delete our Run value out of one user hive.  `who` only names the hive in the
// transcript.
static void gc_remove_tray_autostart_in_hive(HKEY hive, const WCHAR* subKey, const WCHAR* who) {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(hive, subKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) return;  // no Run key for this hive: nothing to do

    WCHAR command[GC_INSTALLER_MAX_PATH_CHARS] = {};
    DWORD bytes = sizeof(command) - sizeof(WCHAR);
    DWORD type = 0;
    status = RegQueryValueExW(key, GC_TRAY_AUTOSTART_VALUE_NAME_W, nullptr, &type,
                              (LPBYTE)command, &bytes);
    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        command[bytes / sizeof(WCHAR)] = 0;
        char commandUtf8[GC_INSTALLER_MAX_PATH_CHARS] = {};
        // A decode failure means the value cannot be shown to be ours, and an
        // entry that cannot be proven ours is never deleted.
        if (gc_wide_to_utf8(command, commandUtf8, (int)sizeof(commandUtf8)) &&
            gc_uninstall_command_references(commandUtf8, GC_SETUP_GUI_EXE)) {
            LONG deleted = RegDeleteValueW(key, GC_TRAY_AUTOSTART_VALUE_NAME_W);
            if (deleted == ERROR_SUCCESS) {
                gc_log_step("autostart: removed the tray startup entry for %ls", who);
            } else {
                gc_log_fail("autostart: the tray startup entry for %ls could not be removed (error %ld)",
                            who, deleted);
            }
        } else {
            gc_log_step("autostart: the \"%ls\" Run value for %ls does not point at this program; leaving it",
                        GC_TRAY_AUTOSTART_VALUE_NAME_W, who);
        }
    }
    RegCloseKey(key);
}

// Remove the resident-tray Run value from every user hive that is loaded.
//
// Hives belonging to accounts that are not signed in are not mounted, and
// mounting them by hand (RegLoadKey over their ntuser.dat) can corrupt a
// profile if that user signs in at the same moment.  Those entries are
// therefore left alone and the limitation is recorded: a Run value pointing at
// a deleted executable is inert, and Windows drops it silently at the next
// logon.
void gc_remove_tray_autostart_values() {
    WCHAR runSubKey[256] = {};
    if (!gc_utf8_to_wide(GC_TRAY_AUTOSTART_RUN_SUBKEY, runSubKey, (int)GC_ARRAY_COUNT(runSubKey))) {
        gc_log_fail("autostart: the Run key path could not be built");
        return;
    }

    int visited = 0;
    for (DWORD index = 0;; index++) {
        WCHAR sid[256] = {};
        DWORD length = GC_ARRAY_COUNT(sid);
        LONG status = RegEnumKeyExW(HKEY_USERS, index, sid, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) {
            gc_log_step("autostart: the loaded user hives could not be listed past %lu (error %ld)",
                        index, status);
            break;
        }
        // Every hive has a "<SID>_Classes" sibling holding per-user COM
        // registrations; it has no Run key and is not a profile root.
        size_t sidLength = wcslen(sid);
        const WCHAR* classesSuffix = L"_Classes";
        size_t suffixLength = wcslen(classesSuffix);
        if (sidLength > suffixLength &&
            _wcsicmp(sid + (sidLength - suffixLength), classesSuffix) == 0) continue;

        WCHAR path[512] = {};
        if (FAILED(StringCchPrintfW(path, GC_ARRAY_COUNT(path), L"%ls\\%ls", sid, runSubKey))) continue;
        gc_remove_tray_autostart_in_hive(HKEY_USERS, path, sid);
        visited++;
    }
    gc_log_step("autostart: checked %d loaded user hive(s) for a tray startup entry; "
                "hives of users who are not signed in are left untouched", visited);
}
