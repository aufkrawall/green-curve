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
#include "linux_cli_profile_load.h"
#include "linux_tui_internal.h"
#include "profile_slot_codec.h"
#include "profile_ini_slot.h"
#include "posix_test_environment.h"
#include "linux_output_directory.h"
#include "linux_config_lock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <memory>

// linux_gpu_target_config.cpp's other half talks to this scripted daemon.
static bool g_daemonReachable;
static bool g_backendStartsOnTargetedRead;
static bool g_serveScriptedSnapshot;
static ServiceSnapshot g_scriptedSnapshot;
static int g_targetedReads;
static GpuAdapterInfo g_liveGpu;
static struct {
    int writes;
    unsigned int mode;
    int slot;
    GpuAdapterInfo target;
    DesiredSettings desired;
} g_startupPolicy;

void linux_debug_logf(const char*, ...) {}
bool linux_daemon_set_startup_policy(unsigned int mode, int slot, const char*,
                                     const GpuAdapterInfo* target, const DesiredSettings* desired,
                                     char*, size_t) {
    ++g_startupPolicy.writes;
    g_startupPolicy.mode = mode;
    g_startupPolicy.slot = slot;
    g_startupPolicy.target = target ? *target : GpuAdapterInfo{};
    g_startupPolicy.desired = desired ? *desired : DesiredSettings{};
    return true;
}
bool tui_configured_gpu_allows_write(TuiState*, const char*) { return true; }
bool tui_refresh_service(TuiState* state, bool, const GpuAdapterInfo*) {
    state->service.state.startupPolicyMode = g_startupPolicy.mode;
    state->service.state.startupPolicySlot = (unsigned int)g_startupPolicy.slot;
    return true;
}
void tui_refresh_startup_snapshot(TuiState* state) {
    state->startupSnapshot = g_startupPolicy.desired;
}
bool linux_daemon_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize) {
    if (g_daemonReachable) {
        if (snapshot) *snapshot = g_serveScriptedSnapshot ? g_scriptedSnapshot : ServiceSnapshot{};
        return true;
    }
    if (err && errSize) snprintf(err, errSize, "%s", "no daemon in this fixture");
    return false;
}
bool linux_daemon_get_state(const GpuAdapterInfo* target, ServiceResponse* response,
                            char* err, size_t errSize) {
    if (g_serveScriptedSnapshot) {
        if (response) {
            *response = ServiceResponse{};
            response->snapshot = g_scriptedSnapshot;
        }
        return true;
    }
    if (target && target->valid) {
        ++g_targetedReads;
        if (g_backendStartsOnTargetedRead) {
            if (response) {
                response->snapshot.adapterCount = 1;
                response->snapshot.adapters[0] = g_liveGpu;
            }
            return true;
        }
    }
    if (err && errSize) snprintf(err, errSize, "%s", "no daemon in this fixture");
    return false;
}

namespace {

void set_daemon_failure(TuiState* state, const char* fallback, const char* detail) {
    linux_tui_format_failure(state->status, sizeof(state->status), fallback, detail);
}
#include "linux_tui_startup_actions.cpp"

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

// A daemon that started before the driver was ready publishes no adapters.
// A read may name config.ini's stored GPU to bring the backend up, but that
// request is not write authority: the same record must resolve against the
// fresh adapter list before the client sends any mutation.
void test_backend_bring_up_on_read() {
    const std::string path = path_in_root("bring-up.ini");
    const GpuAdapterInfo stored = selected_gpu();
    char err[320] = {};
    expect(save_linux_gpu_selection(path.c_str(), &stored,
               GPU_LOCATION_ROLE_REQUIRED, err, sizeof(err)),
           "writing the stored GPU for backend bring-up");
    g_daemonReachable = true;
    g_backendStartsOnTargetedRead = true;
    g_targetedReads = 0;
    g_liveGpu = stored;
    g_liveGpu.valid = g_liveGpu.pciInfoValid = true;
    LinuxConfiguredGpu selection = {};
    linux_configured_gpu_load(path.c_str(), nullptr, &selection);
    const GpuAdapterInfo* readTarget = linux_gpu_target_for_read(
        path.c_str(), &selection);
    expect(g_targetedReads == 1 && readTarget &&
               linux_gpu_identity_matches(readTarget, &g_liveGpu),
           "a stored GPU brings up a deferred backend and resolves for reads");
    expect(selection.status == LINUX_CONFIGURED_GPU_RESOLVED,
           "the freshly published list validates the stored GPU");
    const GpuAdapterInfo* writeTarget = nullptr;
    expect(linux_gpu_target_for_write(path.c_str(), &selection, &writeTarget,
                                      err, sizeof(err)) && writeTarget &&
               linux_gpu_identity_matches(writeTarget, &g_liveGpu),
           "the resolved GPU is then authorized for writes");

    // If the backend stays down, the targeted read is not persisted as proof.
    LinuxConfiguredGpu unavailable = {};
    linux_configured_gpu_load(path.c_str(), nullptr, &unavailable);
    g_backendStartsOnTargetedRead = false;
    g_targetedReads = 0;
    expect(linux_gpu_target_for_read(path.c_str(), &unavailable) == nullptr &&
               g_targetedReads == 1 &&
               unavailable.status == LINUX_CONFIGURED_GPU_UNVALIDATED &&
               !linux_gpu_target_for_write(path.c_str(), &unavailable,
                                           &writeTarget, err, sizeof(err)),
           "an unavailable backend leaves reads untargeted and writes blocked");
}

void test_output_directory_walk() {
    const std::string real = path_in_root("real");
    // The redirect sits in a shared, sticky, world-writable directory like
    // /tmp, so root refuses it even when root created it.
    const std::string shared = path_in_root("shared");
    const std::string link = shared + "/redirect";
    expect(mkdir(real.c_str(), 0700) == 0, "creating the real output directory");
    expect(mkdir(shared.c_str(), 0700) == 0 && chmod(shared.c_str(), 01777) == 0,
           "creating the shared link directory");
    expect(symlink("../real", link.c_str()) == 0, "creating the redirected directory");
    int fd = linux_open_output_directory((link + "/new/child").c_str(), true);
    expect(fd < 0, "privileged ancestor traversal refuses a symlink");
    if (fd >= 0) close(fd);
    struct stat status = {};
    expect(lstat((real + "/new").c_str(), &status) != 0 && errno == ENOENT,
           "refusal creates no directories through a symlink");
    fd = linux_open_output_directory(link.c_str(), true);
    expect(fd < 0, "privileged final directory refuses a symlink");
    if (fd >= 0) close(fd);
    fd = linux_open_output_directory((link + "/user").c_str(), false);
    expect(fd >= 0, "ordinary users retain redirected directory creation");
    if (fd >= 0) close(fd);
    expect(stat((real + "/user").c_str(), &status) == 0,
           "ordinary-user creation reaches the intended redirect");
    char outputError[320] = {};
    const bool outputWritten = write_text_file_atomic((link + "/output/report.ini").c_str(),
        "generated output", outputError, sizeof(outputError));
    expect(outputWritten == (geteuid() != 0),
           "the real atomic writer selects the effective caller's path policy");
    if (geteuid() == 0)
        expect(lstat((real + "/output").c_str(), &status) != 0 && errno == ENOENT,
               "the privileged writer refuses before creating redirected output");
    unlink((real + "/output/report.ini").c_str());
    rmdir((real + "/output").c_str());
    fd = linux_open_output_directory((real + "/./nested//child").c_str(), true);
    expect(fd >= 0, "privileged walk creates nested directories");
    if (fd >= 0) close(fd);
    fd = linux_open_output_directory((real + "/../escape").c_str(), true);
    expect(fd < 0, "privileged walk refuses parent traversal");
    if (fd >= 0) close(fd);
    fd = linux_open_output_directory(".", true);
    expect(fd >= 0, "bare-file output can pin the current directory");
    if (fd >= 0) close(fd);
    const std::string regular = path_in_root("regular");
    expect(write_file(regular, "unchanged"), "creating the regular-file ancestor");
    fd = linux_open_output_directory((regular + "/child").c_str(), true);
    expect(fd < 0 && read_file(regular) == "unchanged",
           "a regular-file ancestor is refused without modifying it");
    if (fd >= 0) close(fd);
    char directoryError[512] = {};
    const std::string blocked = regular + "/child";
    expect(!ensure_directory_recursive(blocked.c_str(), directoryError,
                                       sizeof(directoryError)) &&
               strstr(directoryError, blocked.c_str()) != nullptr,
           "a refused output directory names the requested path");
    // A link root placed in a directory only root can change is a distribution
    // layout (/home -> var/home): root follows it.  The same link owned by
    // anyone else stays refused, so an unprivileged run expects refusal.
    const std::string trusted = path_in_root("trusted");
    expect(symlink("real", trusted.c_str()) == 0, "creating the root-placed link");
    fd = linux_open_output_directory((trusted + "/layout").c_str(), true);
    expect((fd >= 0) == (geteuid() == 0),
           "root follows only a link it owns in a directory only it can change");
    if (fd >= 0) close(fd);
    // A trusted link may itself name a link in a writable directory. The
    // second link must still be checked, and no output may be created there.
    const std::string chain = path_in_root("chain");
    expect(symlink("shared/redirect", chain.c_str()) == 0, "creating a link chain");
    fd = linux_open_output_directory((chain + "/escaped").c_str(), true);
    expect(fd < 0, "privileged walk refuses an untrusted link in a trusted target");
    if (fd >= 0) close(fd);
    expect(lstat((real + "/escaped").c_str(), &status) != 0 && errno == ENOENT,
           "link-chain refusal creates no output");
    unlink(chain.c_str());
    const std::string loop = path_in_root("loop");
    expect(symlink("loop", loop.c_str()) == 0, "creating a link loop");
    fd = linux_open_output_directory(loop.c_str(), true);
    expect(fd < 0, "privileged link resolution is bounded");
    if (fd >= 0) close(fd);
    unlink(loop.c_str());
    unlink(trusted.c_str());
    unlink(regular.c_str());
    unlink(link.c_str());
    rmdir(shared.c_str());
    rmdir((real + "/layout").c_str());
    rmdir((real + "/user").c_str());
    rmdir((real + "/nested/child").c_str());
    rmdir((real + "/nested").c_str());
    rmdir(real.c_str());
}

bool make_root() {
    return gc_test_make_temp_dir(g_root, sizeof(g_root), "gc-gpu-selection", 0) != nullptr;
}

void test_config_storage_lock() {
    const std::string path = path_in_root("lock-proof.ini");
    const std::string lockPath = path + ".lock";
    char why[160]{};
    int probe = -1;
    {
        LinuxConfigLock lock(path.c_str(), why, sizeof(why));
        expect(lock.locked(), "configuration lock acquired");
        probe = open(lockPath.c_str(), O_RDWR | O_CLOEXEC);
        expect(probe >= 0, "configuration lock probe opened");
        if (probe >= 0) expect(flock(probe, LOCK_EX | LOCK_NB) != 0 &&
            (errno == EWOULDBLOCK || errno == EAGAIN), "a separate writer cannot acquire a held lock");
    }
    if (probe >= 0) {
        expect(flock(probe, LOCK_EX | LOCK_NB) == 0, "configuration lock released by its owner");
        close(probe);
    }
    unlink(lockPath.c_str());
}

void test_startup_picker_binding(const std::string& path) {
    auto state = std::make_unique<TuiState>();
    state->serviceOnline = true;
    state->currentSlot = 2;
    state->dirty = true;
    state->desired.hasMemOffset = true;
    state->desired.memOffsetMHz = 77;
    state->targetGpu = selected_gpu();
    state->service.snapshot = g_scriptedSnapshot;
    state->service.state.startupPolicyMode = SERVICE_STARTUP_POLICY_PROFILE;
    state->service.state.startupPolicySlot = 3;
    snprintf(state->configPath, sizeof(state->configPath), "%s", path.c_str());

    open_startup_picker(state.get());
    const bool (&saved)[CONFIG_NUM_SLOTS] = state->startupPicker.savedSlots;
    expect(state->startupPicker.open && state->startupPicker.bankReadable &&
               saved[0] && !saved[1] && saved[2] && saved[3] && saved[4],
           "startup picker lists legacy zero, curve-only, fan-curve-only and imported slots");
    expect(g_startupPolicy.writes == 0, "opening the picker does not change the startup policy");
    expect(apply_startup_policy(state.get(), SERVICE_STARTUP_POLICY_PROFILE, 5) &&
               g_startupPolicy.writes == 1 && state->service.state.startupPolicySlot == 5 &&
               g_startupPolicy.target.pciBus == state->targetGpu.pciBus &&
               state->startupSnapshot.lockCi == 61 && state->startupSnapshot.memOffsetMHz == 3000,
           "startup binding uses the selected Windows profile and maps its lock on the shown GPU");
    expect(state->currentSlot == 2 && state->dirty && state->desired.memOffsetMHz == 77,
           "startup binding leaves the editor draft and slot intact");
    expect(!apply_startup_policy(state.get(), SERVICE_STARTUP_POLICY_PROFILE, 2) &&
               g_startupPolicy.writes == 1 && strstr(state->status, "Profile 2 is empty"),
           "the startup action rejects an empty slot without writing the policy");
    expect(apply_startup_policy(state.get(), SERVICE_STARTUP_POLICY_NONE, 0) &&
               g_startupPolicy.writes == 2 && state->service.state.startupPolicyMode == SERVICE_STARTUP_POLICY_NONE &&
               !g_startupPolicy.target.valid && !g_startupPolicy.desired.hasLock,
           "Nothing clears the startup profile binding");
    expect(apply_startup_policy(state.get(), SERVICE_STARTUP_POLICY_RESTORE_LAST, 0) &&
               g_startupPolicy.writes == 3 && state->service.state.startupPolicyMode == SERVICE_STARTUP_POLICY_RESTORE_LAST,
           "Last applied can be selected directly");
}

void test_voltage_bound_profile_load_and_output() {
    const std::string path = path_in_root("voltage-bound.ini");
    ProfileDocument doc = profile_document_make_empty();
    doc.documentId = 5;
    doc.source.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    snprintf(doc.displayName, sizeof(doc.displayName), "%s", "Voltage-bound");
    doc.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK |
                  PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
    doc.applicationScope = PROFILE_SCOPE_UPDATE_INCLUDED;
    doc.pointSetPolicy = PROFILE_POINTS_REPLACE;
    doc.legacyIndicesUnresolved = false;
    doc.sourceTopologyComplete = false;
    doc.topologyCount = 2;
    doc.topology[0].topologyId = 1;
    doc.topology[0].sourceIndex = 75;
    doc.topology[0].voltageUv = 895000;
    doc.topology[1].topologyId = 2;
    doc.topology[1].sourceIndex = 76;
    doc.topology[1].voltageUv = 900000;
    doc.points[0].kind = PROFILE_POINT_CAPTURED_OFFSET;
    doc.points[0].capturedOffsetHz = 0;
    doc.lock.present = true;
    doc.lock.anchorTopologyId = 2;
    doc.lock.mode = LOCK_MODE_FLATTEN;
    doc.lock.targetFrequencyHz = 2962000000LL;
    doc.hasMemoryOffset = true;
    doc.memoryOffsetHz = 3000000000LL;
    doc.memorySurface = 1u;
    doc.power.present = true;
    doc.power.ratio = tuning_power_ratio_reduce(17, 20);

    std::string slotSection;
    expect(profile_slot_encode(doc, 5, slotSection, nullptr),
           "voltage-bound profile document encodes into INI section");

    std::string iniText = "[profiles]\nselected_slot=5\n[meta]\nlinux_mem_migrated=1\n"
        "[controls]\nmem_offset_mhz=0\n[profile2]\n# empty\n"
        "[profile3_curve]\npoint40=1500\n[profile4_fan_curve]\npoint0=30,20,1\n" + slotSection;
    FILE* wf = fopen(path.c_str(), "wb");
    expect(wf != nullptr, "voltage-bound config.ini opened for write");
    if (wf) {
        fwrite(iniText.data(), 1, iniText.size(), wf);
        fclose(wf);
    }

    char err[256] = {};
    expect(load_selected_profile_slot(path.c_str()) == 5,
           "load_selected_profile_slot reads selected_slot without materializing slot");

    DesiredSettings desired{};
    bool needsLive = false;
    expect(!load_profile_from_config_path_for_destination(
               path.c_str(), 5, nullptr, &desired, err, sizeof(err), &needsLive) &&
               needsLive,
           "offline load of voltage-bound slot reports needsLive=true");

    g_serveScriptedSnapshot = true;
    g_scriptedSnapshot = ServiceSnapshot{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        // Strictly monotonic voltages where 895000 uV lands at index 60 and
        // 900000 uV lands at index 61 (shifted from sourceIndex 75 and 76) to
        // prove voltage-based resolution rather than raw sourceIndex fallback.
        g_scriptedSnapshot.curve[i].volt_uV = 595000u + (unsigned int)i * 5000u;
        g_scriptedSnapshot.curve[i].freq_kHz = 1200000u + (unsigned int)i * 20000u;
        g_scriptedSnapshot.freqOffsets[i] = 0;
    }

    desired = DesiredSettings{};
    expect(linux_load_profile_with_daemon_fallback(
               path.c_str(), 5, nullptr, &desired, err, sizeof(err)),
           "daemon-fallback load resolves voltage-bound slot 5");
    expect(desired.curvePointCaptured[60] == 1 && desired.curvePointCapturedOffsetHz[60] == 0 &&
               desired.curvePointCaptured[75] == 0,
           "captured point resolves by voltage to destination index 60");
    expect(desired.hasLock && desired.lockCi == 61 &&
               desired.lockMode == LOCK_MODE_FLATTEN && desired.lockMHz == 2962,
           "flatten lock resolves by voltage to destination index 61 @ 2962 MHz");

    test_startup_picker_binding(path);

    int selectedSlot = 0;
    DesiredSettings selectedDesired{};
    expect(linux_load_default_or_selected_profile_with_daemon_fallback(
               path.c_str(), &selectedSlot, nullptr, &selectedDesired, err, sizeof(err)) &&
               selectedSlot == 5 && selectedDesired.lockCi == 61,
           "default/selected loader also resolves voltage-bound selected slot via daemon");

    const std::string textPath = path_in_root("dump.txt");
    FILE* tf = fopen(textPath.c_str(), "wb");
    expect(tf != nullptr, "text dump temp file opened");
    if (tf) {
        print_desired_settings_text(tf, 5, &desired);
        fclose(tf);
    }
    std::string textOut;
    if (FILE* rf = fopen(textPath.c_str(), "rb")) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        textOut.assign(buf, n);
        fclose(rf);
    }
    expect(textOut.find("Lock: point 61 @ 2962 MHz (flatten)") != std::string::npos &&
               textOut.find("point60=captured(+0 Hz)") != std::string::npos,
           "text dump prints lock mode and captured point offsets");

    const std::string jsonPath = path_in_root("dump.json");
    FILE* jf = fopen(jsonPath.c_str(), "wb");
    expect(jf != nullptr, "json dump temp file opened");
    if (jf) {
        print_desired_settings_json(jf, 5, &desired);
        fclose(jf);
    }
    std::string jsonOut;
    if (FILE* rf = fopen(jsonPath.c_str(), "rb")) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        jsonOut.assign(buf, n);
        fclose(rf);
    }
    expect(jsonOut.find("{\"index\": 60, \"captured_offset_hz\": 0}") != std::string::npos,
           "json dump emits captured_offset_hz for captured zero-offset points");

    g_serveScriptedSnapshot = false;
    g_scriptedSnapshot = ServiceSnapshot{};
    unlink(textPath.c_str());
    unlink(jsonPath.c_str());

    // Saving an unlocked profile (whose default lockTracksAnchor is true) must
    // write lock_tracks_anchor=0 so Linux -> Windows export succeeds.
    const std::string unlockedPath = path_in_root("unlocked.ini");
    DesiredSettings unlocked{};
    initialize_desired_settings_defaults(&unlocked);
    unlocked.hasGpuOffset = 1;
    unlocked.gpuOffsetMHz = 150;
    unlocked.hasMemOffset = 1;
    unlocked.memOffsetMHz = 500;
    expect(save_profile_to_config_path(unlockedPath.c_str(), 1, &unlocked, err, sizeof(err)),
           "unlocked profile saved to Linux config.ini");
    std::string unlockedText;
    if (FILE* rf = fopen(unlockedPath.c_str(), "rb")) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        unlockedText.assign(buf, n);
        fclose(rf);
    }
    expect(unlockedText.find("lock_tracks_anchor=0") != std::string::npos &&
               unlockedText.find("lock_tracks_anchor=1") == std::string::npos,
           "unlocked Linux save writes lock_tracks_anchor=0");
    auto exportedBank = std::make_unique<ProfileBank>();
    ProfileLegacyOptions linuxOpts{};
    linuxOpts.format = PROFILE_LEGACY_LINUX_INI;
    linuxOpts.nativeSlot = true;
    ProfileCodecError decodeErr{};
    expect(profile_legacy_ini_decode(unlockedText, linuxOpts, ProfileSourceSelection{},
                                     *exportedBank, &decodeErr) &&
               exportedBank->count == 1 && !exportedBank->documents[0].lock.present &&
               exportedBank->documents[0].graphicsOffset.offsetHz == 150000000LL,
           "unlocked Linux config.ini decodes cleanly for export to Windows");
    unlink(unlockedPath.c_str());
    unlink((unlockedPath + ".lock").c_str());
}

void remove_root() {
    const char* names[] = {"missing.ini", "profiles.ini", "oversized.ini",
                           "locked.ini", "bring-up.ini", "voltage-bound.ini"};
    for (const char* name : names) {
        unlink(path_in_root(name).c_str());
        unlink((path_in_root(name) + ".lock").c_str());
    }
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
    test_backend_bring_up_on_read();
    test_output_directory_walk();
    test_config_storage_lock();
    test_voltage_bound_profile_load_and_output();
    remove_root();
    if (g_failures != 0) {
        fprintf(stderr, "Linux GPU selection store regression FAILED (%d)\n",
                g_failures);
        return 1;
    }
    printf("Linux GPU selection store regression passed\n");
    return 0;
}
