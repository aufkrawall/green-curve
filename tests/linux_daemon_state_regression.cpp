// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the production file loader/store without requiring a root test job.
#include "linux_daemon_state.h"
#include "desired_settings_ui_policy.h"
#include "platform.h"
#include "posix_test_environment.h"
#include <fcntl.h>
#include <sys/stat.h>

static char fixtureRoot[4096];
static int fixture_fstat(int fd, struct stat* status) {
    const int result = ::fstat(fd, status);
    if (result != 0 || status->st_uid != geteuid()) return result;
    char descriptor[64]{}, path[4096]{};
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    const ssize_t count = readlink(descriptor, path, sizeof(path) - 1);
    if (count < 0) return result;
    path[count] = 0;
    const size_t rootSize = strlen(fixtureRoot);
    if (rootSize && !strncmp(path, fixtureRoot, rootSize) &&
        (!path[rootSize] || path[rootSize] == '/')) {
        // Only this fixture's owned scratch tree gets synthetic root identity.
        // Modes, inode types, links, reads, fsync and replacement stay real.
        status->st_uid = 0;
    }
    return result;
}
static int fixture_fchown(int fd, uid_t owner, gid_t group) {
    if (geteuid() != 0 && owner == 0 && group == 0) {
        struct stat actual{}, adapted{};
        if (::fstat(fd, &actual) == 0 && actual.st_uid == geteuid() &&
            fixture_fstat(fd, &adapted) == 0 && adapted.st_uid == 0)
            return ::fchown(fd, geteuid(), getegid());
    }
    return ::fchown(fd, owner, group);
}
#define fstat fixture_fstat
#define fchown fixture_fchown
#include "linux_daemon_state.cpp"
#undef fstat
#undef fchown

struct Scratch {
    char path[4096]{};
    Scratch() {
        if (gc_test_make_temp_dir(fixtureRoot, sizeof(fixtureRoot), "greencurve-state", 128))
            snprintf(path, sizeof(path), "%s/active.bin", fixtureRoot);
    }
    ~Scratch() {
        if (*path) unlink(path);
        if (*fixtureRoot) rmdir(fixtureRoot);
    }
};
template<class T> static bool write_fixture(const char* path, const T& value) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = write_all(fd, &value, sizeof(value));
    return close(fd) == 0 && ok;
}
template<class T> static bool read_fixture(const char* path, T& value) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const ssize_t count = gc_read_record(::read, fd, &value, sizeof(value));
    char extra = 0;
    const bool ok = count == (ssize_t)sizeof(value) && read(fd, &extra, 1) == 0;
    return close(fd) == 0 && ok;
}
int main() {
    Scratch scratch;
    if (!*scratch.path) return 1;
    GpuAdapterInfo gpu{}; gpu.valid = gpu.pciInfoValid = 1;
    gpu.pciDomain = 2; gpu.pciBus = 3;
    DesiredSettings desired{}; desired.hasGpuOffset = 1; desired.gpuOffsetMHz = -17;
    desired.authoring.graphicsPrecise = 1; desired.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    desired.authoring.graphicsOffsetHz = -17000000;
    LinuxDaemonStateRecordSchema7 old{};
    old.magic = LINUX_DAEMON_RECORD_MAGIC; old.version = LINUX_DAEMON_RECORD_PRE_GRAPHICS_RECOVERY_VERSION;
    old.size = sizeof(old); old.state = LINUX_DAEMON_RECORD_ACTIVE;
    old.targetGpu = gpu; old.targetLocationRole = GPU_LOCATION_ROLE_REQUIRED;
    desired_settings_narrow_schema5(&old.desired, &desired);
    old.checksum = linux_daemon_record_hash_bytes(&old, offsetof(LinuxDaemonStateRecordSchema7, checksum));
    memset((unsigned char*)&old + offsetof(LinuxDaemonStateRecordSchema7, checksum) + sizeof(old.checksum),
        0xff, sizeof(old) - offsetof(LinuxDaemonStateRecordSchema7, checksum) - sizeof(old.checksum));
    if (!write_fixture(scratch.path, old)) return 2;
    LinuxDaemonStateRecord loaded{}; char why[512]{}; bool migrated = false;
    if (linux_daemon_state_load(scratch.path, &loaded, why, sizeof(why), &migrated) != LINUX_DAEMON_STATE_LOADED ||
        !migrated || loaded.graphicsControlSurfaces || loaded.version != LINUX_DAEMON_RECORD_VERSION ||
        !desired_settings_equal(&desired, &loaded.desired)) return 3;
    LinuxDaemonStateRecordSchema7 reread{};
    if (!read_fixture(scratch.path, reread) || memcmp(&reread, &old, sizeof(old))) return 4;
    LinuxDaemonStateRecord prepared{};
    linux_daemon_record_initialize(&prepared, LINUX_DAEMON_RECORD_PREPARED, &gpu, &desired,
        51, SERVICE_OPERATION_IN_PROGRESS, GPU_LOCATION_ROLE_REQUIRED, nullptr, GRAPHICS_SCALAR_RECOVERY_ALL);
    if (!linux_daemon_state_store(scratch.path, &prepared, why, sizeof(why))) {
        fprintf(stderr, "daemon state fixture: prepared store refused: %s\n", why);
        return 5;
    }
    if (linux_daemon_state_load(scratch.path, &loaded, why, sizeof(why), &migrated) != LINUX_DAEMON_STATE_LOADED ||
        migrated || memcmp(&loaded, &prepared, sizeof(loaded))) return 6;
    const auto uncertain = linux_daemon_record_as_uncertain(&loaded);
    if (uncertain.graphicsControlSurfaces != GRAPHICS_SCALAR_RECOVERY_ALL ||
        !linux_daemon_state_store(scratch.path, &uncertain, why, sizeof(why))) return 7;
    prepared.recoveryReserved = 1; prepared.checksum = linux_daemon_record_checksum(&prepared);
    if (linux_daemon_state_store(scratch.path, &prepared, why, sizeof(why)) ||
        !read_fixture(scratch.path, loaded) || memcmp(&loaded, &uncertain, sizeof(loaded))) return 8;
    loaded.checksum ^= 1;
    if (!write_fixture(scratch.path, loaded) ||
        linux_daemon_state_load(scratch.path, &loaded, why, sizeof(why), &migrated) != LINUX_DAEMON_STATE_INVALID_REMOVED ||
        access(scratch.path, F_OK) == 0) return 9;
    if (!write_fixture(scratch.path, old) || chmod(scratch.path, 0644)) return 10;
    if (linux_daemon_state_load(scratch.path, &loaded, why, sizeof(why), &migrated) != LINUX_DAEMON_STATE_INVALID_REMOVED ||
        access(scratch.path, F_OK) == 0) return 11;
    puts("Linux daemon state files: frozen migration, durable coverage, invalid-store preservation and rejection passed");
    return 0;
}
