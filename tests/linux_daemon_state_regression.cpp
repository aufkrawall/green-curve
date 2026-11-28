// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Exercise the production file loader/store without requiring a root test job.
#include "linux_daemon_state.h"
#include "desired_settings_ui_policy.h"
#include "platform.h"
#include "linux_service_install_files.h"
#include "posix_test_environment.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
static bool stage_installer_fixture(LinuxInstallFile& file, char contents) {
    const int fd = file.stage(0600);
    if (fd < 0) return false;
    const bool wrote = write_all(fd, &contents, sizeof(contents)) && fsync(fd) == 0;
    return close(fd) == 0 && wrote;
}

static int run_install_files_fixture(const char* path) {
    LinuxServiceInstallLock first(fixtureRoot);
    LinuxServiceInstallLock second(fixtureRoot);
    if (!first.locked() || second.locked()) return 12;
    const std::string newPath = std::string(path) + ".resume";
    if (!write_fixture(path, 'o')) return 13;
    LinuxInstallFile existing(path, geteuid(), getegid());
    LinuxInstallFile fresh(newPath.c_str(), geteuid(), getegid());
    if (!stage_installer_fixture(existing, 'u') || !stage_installer_fixture(fresh, 'n') ||
        !existing.save_previous() || !fresh.save_previous()) return 14;
    char observed = 0;
    if (!read_fixture(path, observed) || observed != 'o' || access(newPath.c_str(), F_OK) == 0)
        return 15;
    const std::string oldBackup = existing.backup();
    if (!existing.publish() || !fresh.publish() ||
        !read_fixture(path, observed) || observed != 'u' ||
        !read_fixture(newPath.c_str(), observed) || observed != 'n') {
        fresh.restore();
        existing.restore();
        return 16;
    }
    // Activation rejected the replacement: recover the installed file and
    // remove the unit that only the failed upgrade introduced.
    if (!fresh.restore() || !existing.restore() || !read_fixture(path, observed) ||
        observed != 'o' || access(newPath.c_str(), F_OK) == 0 || access(oldBackup.c_str(), F_OK) == 0)
        return 17;

    LinuxInstallFile committed(path, geteuid(), getegid());
    if (!stage_installer_fixture(committed, 'c') || !committed.save_previous()) return 18;
    const std::string committedBackup = committed.backup();
    if (!committed.publish() || !committed.commit() ||
        !read_fixture(path, observed) || observed != 'c' || access(committedBackup.c_str(), F_OK) == 0)
        return 19;

    // A publish failure after another file was replaced can restore the
    // earlier file. A directory occupying the second fresh target refuses rename.
    LinuxInstallFile firstFile(path, geteuid(), getegid());
    LinuxInstallFile blocked(newPath.c_str(), geteuid(), getegid());
    if (!stage_installer_fixture(firstFile, 'x') || !stage_installer_fixture(blocked, 'y') ||
        !firstFile.save_previous() || !blocked.save_previous() || mkdir(newPath.c_str(), 0700))
        return 20;
    const bool rejected = firstFile.publish() && !blocked.publish();
    const bool restored = firstFile.restore();
    if (rmdir(newPath.c_str()) || !rejected || !restored ||
        !read_fixture(path, observed) || observed != 'c') return 21;
    // Staged files carry the explicit owner, group and mode, never the process's
    // umask or a setgid parent's group. Production passes root:root; the
    // unprivileged fixture can only chown to a group it belongs to.
    gid_t groups[64] = {};
    const int groupCount = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);
    gid_t otherGroup = getegid();
    for (int i = 0; i < groupCount; ++i)
        if (groups[i] != getegid()) { otherGroup = groups[i]; break; }
    LinuxInstallFile owned(path, geteuid(), otherGroup);
    const int ownedFd = owned.stage(0640);
    struct stat ownedStat = {};
    const bool ownedOk = ownedFd >= 0 && fstat(ownedFd, &ownedStat) == 0 &&
        ownedStat.st_uid == geteuid() && ownedStat.st_gid == otherGroup &&
        (ownedStat.st_mode & 07777) == 0640;
    if (ownedFd >= 0) close(ownedFd);
    if (!ownedOk) return 22;
    // Root-only ownership is the production default.
    LinuxInstallFile rootDefault(path);
    const int rootFd = rootDefault.stage(0644);
    if (geteuid() == 0) {
        struct stat rootStat = {};
        const bool rootOk = rootFd >= 0 && fstat(rootFd, &rootStat) == 0 &&
            rootStat.st_uid == 0 && rootStat.st_gid == 0;
        if (rootFd >= 0) close(rootFd);
        if (!rootOk) return 23;
    } else if (rootFd >= 0) {
        // An unprivileged process cannot hand a file to root, so staging must
        // refuse instead of silently leaving it owned by the caller.
        close(rootFd);
        return 24;
    }
    puts("Linux installer files: staging, lock, activation rollback, partial publish and commit passed");
    return 0;
}

static bool daemon_instance_lock_fixture(const char* statePath) {
    char why[192]{};
    const int first = linux_daemon_lock_instance(statePath, why, sizeof(why));
    if (first < 0) return false;
    const int duplicate = linux_daemon_lock_instance(statePath, why, sizeof(why));
    const bool duplicateRefused = duplicate < 0 && strstr(why, "instance lock");
    if (duplicate >= 0) close(duplicate);
    close(first);
    const int replacement = linux_daemon_lock_instance(statePath, why, sizeof(why));
    if (replacement < 0) return false;
    close(replacement);
    return duplicateRefused;
}

int main() {
    Scratch scratch;
    if (!*scratch.path) return 1;
    if (!daemon_instance_lock_fixture(scratch.path)) return 25;
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
    // A journal written by a daemon that runs several GPUs (k records back to
    // back) is kept: reported as several records, never deleted as an
    // unknown layout, and neither replaced nor removed by this daemon.
    struct TwoRecords { LinuxDaemonStateRecord first, second; } pair{};
    pair.first = uncertain;
    GpuAdapterInfo second = gpu; second.pciBus = 4;
    linux_daemon_record_initialize(&pair.second, LINUX_DAEMON_RECORD_ACTIVE, &second, &desired,
        0, SERVICE_OPERATION_NONE, GPU_LOCATION_ROLE_REQUIRED, nullptr, 0);
    if (!write_fixture(scratch.path, pair)) return 12;
    if (linux_daemon_state_load(scratch.path, &loaded, why, sizeof(why), &migrated) != LINUX_DAEMON_STATE_MULTIPLE ||
        !strstr(why, "2 GPU records")) return 13;
    if (linux_daemon_state_store(scratch.path, &prepared, why, sizeof(why)) ||
        linux_daemon_state_store(scratch.path, &uncertain, why, sizeof(why)) ||
        linux_daemon_state_remove(scratch.path, why, sizeof(why))) return 14;
    TwoRecords kept{};
    if (!read_fixture(scratch.path, kept) || memcmp(&kept, &pair, sizeof(pair))) return 15;
    if (unlink(scratch.path) != 0) return 16;
    puts("Linux daemon state files: instance lock, frozen migration, durable coverage, invalid-record rejection and multi-GPU journal preservation passed");
    return run_install_files_fixture(scratch.path);
}
