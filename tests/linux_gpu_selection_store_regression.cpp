// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Native Linux filesystem coverage for config.ini's [gpu] record
// (save_linux_gpu_selection / load_linux_gpu_selection).
//
// The bug this exists to keep fixed: saving the selection started from an
// EMPTY document whenever config.ini could not be read, so the rewrite kept
// only [gpu] and replaced every profile in the file.  An unreadable config.ini
// loads as a MALFORMED selection, whose remedy is `--gpu` -- exactly that save.
// The invariant, from both sides:
//
//     a save rewrites config.ini only when it could read it first, and then
//     keeps every other section.

#include "linux_port.h"
#include "linux_port_internal.h"
#include "posix_test_environment.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

// linux_gpu_target_config.cpp's other half talks to the daemon; these tests
// only save and load the record, so the client and the log are inert.
void linux_debug_logf(const char*, ...) {}
bool linux_daemon_snapshot(ServiceSnapshot*, char* err, size_t errSize) {
    if (err && errSize) snprintf(err, errSize, "%s", "no daemon in this fixture");
    return false;
}

namespace {

int g_failures = 0;
char g_root[256] = {};

void expect(bool condition, const char* what) {
    if (condition) return;
    ++g_failures;
    fprintf(stderr, "FAIL: %s\n", what);
}

std::string path_in_root(const char* name) {
    return std::string(g_root) + "/" + name;
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    return fclose(file) == 0 && ok;
}

std::string read_file(const std::string& path) {
    std::string text;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return text;
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, count);
    fclose(file);
    return text;
}

GpuAdapterInfo selected_gpu() {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciInfoValid = true;
    gpu.pciBus = 7;
    gpu.deviceId = 0x2F0410DEu;
    gpu.subSystemId = 0x89E71043u;
    return gpu;
}

// No config.ini yet: the save creates one, and the record loads back.
void test_missing_file_is_created() {
    const std::string path = path_in_root("missing.ini");
    const GpuAdapterInfo gpu = selected_gpu();
    char err[320] = {};
    expect(save_linux_gpu_selection(path.c_str(), &gpu,
               GPU_LOCATION_ROLE_REQUIRED, err, sizeof(err)),
           "a missing config.ini is created");
    GpuAdapterInfo loaded = {};
    GpuLocationRole role = GPU_LOCATION_ROLE_UNRECORDED;
    expect(load_linux_gpu_selection(path.c_str(), &loaded, &role) ==
               LINUX_GPU_SELECTION_RECORD_PRESENT &&
               loaded.pciBus == 7 && role == GPU_LOCATION_ROLE_REQUIRED,
           "the created record loads back with its location and role");
}

// A readable config.ini keeps every other section across the save.
void test_readable_file_keeps_profiles() {
    const std::string path = path_in_root("profiles.ini");
    expect(write_file(path, "[profiles]\nselected_slot=2\n[profile2]\n"
                            "gpu_offset_mhz=150\n"),
           "writing the profile fixture");
    const GpuAdapterInfo gpu = selected_gpu();
    char err[320] = {};
    expect(save_linux_gpu_selection(path.c_str(), &gpu,
               GPU_LOCATION_ROLE_ADVISORY, err, sizeof(err)),
           "a readable config.ini takes the selection");
    const std::string text = read_file(path);
    expect(text.find("gpu_offset_mhz=150") != std::string::npos &&
               text.find("selected_slot=2") != std::string::npos,
           "the profiles survive the selection save");
    expect(text.find("selected_pci_location_role=advisory") != std::string::npos,
           "the selection is written next to them");
}

// The bug: a config.ini over the INI limit cannot be read.  It loads as a
// MALFORMED selection, and the save -- `--gpu`, the remedy -- used to replace
// it with a file holding only [gpu].  It now fails and leaves it byte for byte.
void test_unreadable_file_is_not_overwritten() {
    const std::string path = path_in_root("oversized.ini");
    std::string text = "[profile1]\ngpu_offset_mhz=90\n";
    text.append((size_t)LINUX_INI_MAX_BYTES, ';');
    text.append("\n");
    expect(write_file(path, text), "writing the oversized fixture");
    GpuAdapterInfo loaded = {};
    expect(load_linux_gpu_selection(path.c_str(), &loaded, nullptr) ==
               LINUX_GPU_SELECTION_RECORD_MALFORMED,
           "an unreadable config.ini is a malformed selection, not none");
    const GpuAdapterInfo gpu = selected_gpu();
    char err[320] = {};
    expect(!save_linux_gpu_selection(path.c_str(), &gpu,
               GPU_LOCATION_ROLE_REQUIRED, err, sizeof(err)),
           "saving into an unreadable config.ini fails");
    expect(strstr(err, "not overwritten") != nullptr,
           "the failure says the file was left alone");
    expect(read_file(path) == text,
           "the unreadable config.ini is unchanged, profiles included");
}

// Permission-denied is the other unreadable shape.  Root reads through the
// mode bits, so the case only exists for an unprivileged run.
void test_unreadable_permissions_are_not_overwritten() {
    if (geteuid() == 0) return;
    const std::string path = path_in_root("locked.ini");
    const std::string text = "[profile1]\ngpu_offset_mhz=60\n";
    expect(write_file(path, text), "writing the locked fixture");
    expect(chmod(path.c_str(), 0200) == 0, "making the fixture unreadable");
    const GpuAdapterInfo gpu = selected_gpu();
    char err[320] = {};
    expect(!save_linux_gpu_selection(path.c_str(), &gpu,
               GPU_LOCATION_ROLE_REQUIRED, err, sizeof(err)),
           "saving into an unreadable (mode 0200) config.ini fails");
    chmod(path.c_str(), 0600);
    expect(read_file(path) == text,
           "the unreadable (mode 0200) config.ini is unchanged");
}

bool make_root() {
    return gc_test_make_temp_dir(g_root, sizeof(g_root), "gc-gpu-selection", 0) != nullptr;
}

void remove_root() {
    const char* names[] = {"missing.ini", "profiles.ini", "oversized.ini",
                           "locked.ini"};
    for (const char* name : names) unlink(path_in_root(name).c_str());
    rmdir(g_root);
}

} // namespace

int main() {
    if (!make_root()) {
        fprintf(stderr, "FAIL: cannot create the fixture directory\n");
        return 1;
    }
    test_missing_file_is_created();
    test_readable_file_keeps_profiles();
    test_unreadable_file_is_not_overwritten();
    test_unreadable_permissions_are_not_overwritten();
    remove_root();
    if (g_failures != 0) {
        fprintf(stderr, "Linux GPU selection store regression FAILED (%d)\n",
                g_failures);
        return 1;
    }
    printf("Linux GPU selection store regression passed\n");
    return 0;
}
