// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Windows Error Reporting LocalDumps registration for our own executables.
//
// Green Curve writes its own minidumps from the unhandled-exception filter and,
// through fatal_dump_hook.h, just before a fast-fail that OUR code raises.  A
// fast-fail raised anywhere else -- ntdll's own CFG dispatch, a CET shadow-stack
// mismatch, heap-corruption detection, or a /GS failure inside a GPU driver DLL
// -- reaches neither path: it is STATUS_STACK_BUFFER_OVERRUN (0xC0000409) and
// goes straight to the kernel.  Only Windows Error Reporting sees those, and
// WER keeps nothing on disk unless a LocalDumps key exists for the image name.
// So the service install registers one, and removal takes it away again.
//
// The key is per image name and machine-wide, and an administrator may already
// have one for a reason of their own.  We therefore only ever write or delete a
// key that carries our marker value; a foreign key is left exactly as it is.
//
// Pure decisions only; the registry I/O lives in wer_local_dumps_win.h.
#ifndef GREEN_CURVE_WER_LOCAL_DUMPS_POLICY_H
#define GREEN_CURVE_WER_LOCAL_DUMPS_POLICY_H

#define GC_WER_LOCAL_DUMPS_KEY \
    L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps"
// Marker value: present only on keys this program created.
#define GC_WER_MARKER_VALUE L"GreenCurveManaged"
// Bounded: WER itself deletes the oldest beyond DumpCount.
#define GC_WER_DUMP_COUNT 5u
// DumpType 0 = custom: CustomDumpFlags decides, so the WER dump carries the same
// streams as the program's own (main_crash_artifacts.cpp,
// green_curve_actionable_minidump_type): DataSegs 0x1, HandleData 0x4,
// ScanMemory 0x10, UnloadedModules 0x20 (the evidence for "a driver DLL was
// swapped under us"), IndirectlyReferencedMemory 0x40, ProcessThreadData 0x100,
// FullMemoryInfo 0x800, ThreadInfo 0x1000.  No full process memory.
#define GC_WER_DUMP_TYPE_CUSTOM 0u
#define GC_WER_CUSTOM_DUMP_FLAGS 0x1975u

enum GcWerKeyState {
    GC_WER_KEY_ABSENT = 0,
    GC_WER_KEY_OURS,      // exists and carries GC_WER_MARKER_VALUE
    GC_WER_KEY_FOREIGN,   // exists without our marker
    GC_WER_KEY_UNKNOWN,   // could not be inspected
};

enum GcWerAction {
    GC_WER_ACTION_WRITE = 0,  // create or refresh our values
    GC_WER_ACTION_DELETE,     // remove our key
    GC_WER_ACTION_LEAVE,      // touch nothing
};

// Registration: ours or absent is (re)written; anything else is left alone.
static inline GcWerAction gc_wer_register_action(GcWerKeyState state) {
    return (state == GC_WER_KEY_ABSENT || state == GC_WER_KEY_OURS)
        ? GC_WER_ACTION_WRITE : GC_WER_ACTION_LEAVE;
}

// Removal: only a key we can prove is ours is deleted.
static inline GcWerAction gc_wer_unregister_action(GcWerKeyState state) {
    return state == GC_WER_KEY_OURS ? GC_WER_ACTION_DELETE : GC_WER_ACTION_LEAVE;
}

static inline const char* gc_wer_key_state_name(GcWerKeyState state) {
    switch (state) {
        case GC_WER_KEY_ABSENT: return "absent";
        case GC_WER_KEY_OURS: return "ours";
        case GC_WER_KEY_FOREIGN: return "foreign";
        default: return "unknown";
    }
}

// The images covered and where WER puts their dumps.  The service runs as
// LocalSystem: its dumps go to a subfolder of its own admin-only data directory
// (main_data_paths.cpp resolve_service_machine_data_dir), never a world-readable
// place, because a SYSTEM dump can hold another account's data.  The GUI keeps
// WER's per-user default (%LOCALAPPDATA%\CrashDumps), expanded by WER in the
// crashing user's context, so a null folder means "do not set DumpFolder".
struct GcWerImage {
    const wchar_t* imageName;
    const wchar_t* dumpFolder;  // REG_EXPAND_SZ, or nullptr for WER's default
};

static const GcWerImage kGcWerImages[] = {
    { L"greencurve-service.exe",
      L"%SystemRoot%\\System32\\config\\systemprofile\\AppData\\Local\\Green Curve\\WerDumps" },
    { L"greencurve.exe", nullptr },
};
#define GC_WER_IMAGE_COUNT (sizeof(kGcWerImages) / sizeof(kGcWerImages[0]))

#endif  // GREEN_CURVE_WER_LOCAL_DUMPS_POLICY_H
