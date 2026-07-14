// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Remove artifacts from the retired startup-inference and nonce-less ticket
// designs. No service-start path can become a synthetic logon/recovery event.
static void service_cleanup_obsolete_recovery_artifacts() {
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return;
    char path[MAX_PATH] = {};
    if (SUCCEEDED(StringCchPrintfA(path, ARRAY_COUNT(path),
            "%s\\service_boot_reconcile.bin", dir))) gc_DeleteFileUtf8(path);
    if (SUCCEEDED(StringCchPrintfA(path, ARRAY_COUNT(path),
            "%s\\service_boot_start.bin", dir))) gc_DeleteFileUtf8(path);
    if (SUCCEEDED(StringCchPrintfA(path, ARRAY_COUNT(path),
            "%s\\service_controlled_recovery.bin", dir))) gc_DeleteFileUtf8(path);
}
