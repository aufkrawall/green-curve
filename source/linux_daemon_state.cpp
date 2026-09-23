// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_daemon_state.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>

static bool write_all(int fd, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(fd, bytes + written, size - written);
        if (count > 0) { written += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool split_path(const char* path, char* directory, size_t directorySize,
                       char* name, size_t nameSize) {
    if (!path || !*path) return false;
    const char* slash = strrchr(path, '/');
    if (!slash || slash == path || !slash[1]) return false;
    size_t dirLength = (size_t)(slash - path);
    if (dirLength >= directorySize || strlen(slash + 1) >= nameSize) return false;
    memcpy(directory, path, dirLength);
    directory[dirLength] = 0;
    gc_strlcpy(name, nameSize, slash + 1);
    return true;
}

static int open_state_directory(const char* path, char* name, size_t nameSize,
                                char* err, size_t errSize) {
    char directory[4096] = {};
    if (!split_path(path, directory, sizeof(directory), name, nameSize)) {
        gc_snprintf(err, errSize, "invalid daemon state path: %s", path ? path : "<null>");
        return -1;
    }
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        gc_snprintf(err, errSize, "cannot create state directory %s: %s", directory, strerror(errno));
        return -1;
    }
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        gc_snprintf(err, errSize, "cannot open state directory %s: %s", directory, strerror(errno));
        return -1;
    }
    struct stat st = {};
    if (fstat(dirfd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0022) != 0) {
        gc_snprintf(err, errSize, "state directory %s is not root-owned and protected", directory);
        close(dirfd);
        return -1;
    }
    return dirfd;
}

// Shared by the operation and startup records: same-directory temp, file fsync,
// atomic rename, directory fsync, root-owned 0600 throughout.
static bool store_record_atomic(const char* path, const void* record,
                                size_t recordSize, const char* label,
                                char* err, size_t errSize) {
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    bool stored = false;
    for (unsigned int attempt = 0; attempt < 16 && !stored; ++attempt) {
        gc_u64 suffix = 0;
        if (getrandom(&suffix, sizeof(suffix), 0) != (ssize_t)sizeof(suffix)) {
            gc_snprintf(err, errSize, "cannot generate daemon %s temp name: %s",
                        label, strerror(errno));
            break;
        }
        char temporary[320] = {};
        gc_snprintf(temporary, sizeof(temporary), ".%s.tmp.%016llx",
                    name, (unsigned long long)suffix);
        int fd = openat(dirfd, temporary,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            gc_snprintf(err, errSize, "cannot create daemon %s temp: %s",
                        label, strerror(errno));
            break;
        }
        bool ok = fchmod(fd, 0600) == 0 && fchown(fd, 0, 0) == 0 &&
            write_all(fd, record, recordSize) && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok && renameat(dirfd, temporary, dirfd, name) == 0 &&
            fsync(dirfd) == 0) {
            stored = true;
        } else {
            if (err && errSize && !err[0])
                gc_snprintf(err, errSize, "cannot commit daemon %s: %s",
                            label, strerror(errno));
            unlinkat(dirfd, temporary, 0);
        }
    }
    close(dirfd);
    return stored;
}

LinuxDaemonStateLoadResult linux_daemon_state_load(const char* path,
                                                   LinuxDaemonStateRecord* out,
                                                   char* err, size_t errSize,
                                                   bool* outMigratedFromLegacy) {
    if (err && errSize) err[0] = 0;
    if (outMigratedFromLegacy) *outMigratedFromLegacy = false;
    if (out) memset(out, 0, sizeof(*out));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return LINUX_DAEMON_STATE_IO_ERROR;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        if (saved == ENOENT) return LINUX_DAEMON_STATE_MISSING;
        gc_snprintf(err, errSize, "cannot open daemon state: %s", strerror(saved));
        return LINUX_DAEMON_STATE_IO_ERROR;
    }
    struct stat st = {};
    bool protectedRegular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                   st.st_uid == 0 && st.st_nlink == 1 &&
                   (st.st_mode & 0077) == 0;
    // SIZE selects the LAYOUT (see desired_settings_schema.h).  Each known
    // byte count belongs to exactly one frozen record layout; the version
    // field inside it then selects the value migrations, and is never asked to
    // imply a layout -- which is what the old "st_size must equal the CURRENT
    // struct" gate silently depended on, and what deleted every 0.25.2 record
    // the moment DesiredSettings grew.
    LinuxDaemonStateRecord record = {};
    ssize_t count = -1;
    gc_u32 generation = LINUX_DAEMON_RECORD_GENERATION_CURRENT;
    gc_u32 storedVersion = 0;
    int migratedOldMemMHz = 0;
    bool migratedMemConverted = false;
    bool migratedFromLegacy = false;
    if (protectedRegular &&
        st.st_size == (off_t)sizeof(LinuxDaemonStateRecord)) {
        count = read(fd, &record, sizeof(record));
        storedVersion = record.version;
    } else if (protectedRegular &&
               st.st_size == (off_t)sizeof(LinuxDaemonStateRecordSchema1)) {
        LinuxDaemonStateRecordSchema1 legacy = {};
        if (read(fd, &legacy, sizeof(legacy)) == (ssize_t)sizeof(legacy) &&
            linux_daemon_state_record_widen_schema1(&record, &legacy,
                                                    &migratedOldMemMHz,
                                                    &storedVersion)) {
            count = (ssize_t)sizeof(record);
            generation = LINUX_DAEMON_RECORD_GENERATION_SCHEMA1;
            migratedFromLegacy = true;
            migratedMemConverted = migratedOldMemMHz != 0;
        }
    } else if (protectedRegular &&
               st.st_size == (off_t)sizeof(LinuxDaemonStateRecordSchema1V1)) {
        LinuxDaemonStateRecordSchema1V1 legacy = {};
        if (read(fd, &legacy, sizeof(legacy)) == (ssize_t)sizeof(legacy) &&
            linux_daemon_state_record_widen_schema1_v1(&record, &legacy,
                                                       &migratedOldMemMHz)) {
            count = (ssize_t)sizeof(record);
            generation = LINUX_DAEMON_RECORD_GENERATION_SCHEMA1_V1;
            storedVersion = LINUX_DAEMON_RECORD_PRE_OPERATION_ID_VERSION;
            migratedFromLegacy = true;
            migratedMemConverted = migratedOldMemMHz != 0;
        }
    }
    close(fd);
    LinuxDaemonStateLoadResult result = LINUX_DAEMON_STATE_LOADED;
    if (!protectedRegular || count != (ssize_t)sizeof(record) ||
        !linux_daemon_record_valid(&record)) {
        // A bare DesiredSettings dump, in either schema, is the pre-record
        // format rather than a damaged record; keeping the two apart is the
        // difference between "nothing to migrate" and "something went wrong"
        // in a support log.
        bool bareDesiredBlob = S_ISREG(st.st_mode) &&
            (st.st_size == (off_t)sizeof(DesiredSettings) ||
             st.st_size == (off_t)sizeof(DesiredSettingsSchema1));
        if (!protectedRegular) {
            gc_snprintf(err, errSize,
                "daemon state rejected: not a root-owned private regular file "
                "(regular=%d uid=%u links=%u mode=%03o)",
                S_ISREG(st.st_mode) ? 1 : 0, (unsigned int)st.st_uid,
                (unsigned int)st.st_nlink, (unsigned int)(st.st_mode & 07777));
        } else if (count != (ssize_t)sizeof(record)) {
            gc_snprintf(err, errSize,
                "daemon state rejected: %lld bytes matches no known record "
                "layout (current=%zu schema1=%zu schema1v1=%zu)",
                (long long)st.st_size, sizeof(LinuxDaemonStateRecord),
                sizeof(LinuxDaemonStateRecordSchema1),
                sizeof(LinuxDaemonStateRecordSchema1V1));
        } else {
            gc_snprintf(err, errSize,
                "daemon state rejected: record failed validation "
                "(version=%u expected=%u state=%u)",
                (unsigned int)record.version,
                (unsigned int)LINUX_DAEMON_RECORD_VERSION,
                (unsigned int)record.state);
        }
        result = bareDesiredBlob
            ? LINUX_DAEMON_STATE_LEGACY_REMOVED : LINUX_DAEMON_STATE_INVALID_REMOVED;
        if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) {
            gc_snprintf(err, errSize, "cannot remove invalid daemon state: %s", strerror(errno));
            result = LINUX_DAEMON_STATE_IO_ERROR;
        } else {
            fsync(dirfd);
        }
    } else {
        if (out) *out = record;
        if (migratedFromLegacy) {
            // The caller's next state transition rewrites this as the current
            // generation.  Loading stays read-only so startup cannot turn a
            // legacy record into a fresh authorization event.
            char memDetail[64] = {};
            if (migratedMemConverted)
                gc_snprintf(memDetail, sizeof(memDetail),
                            "mem offset %d -> %d display MHz",
                            migratedOldMemMHz, record.desired.memOffsetMHz);
            gc_snprintf(err, errSize,
                "loaded %lld-byte %s daemon state v%u -> v%u; %s",
                (long long)st.st_size,
                generation == LINUX_DAEMON_RECORD_GENERATION_SCHEMA1_V1
                    ? "pre-operation-id schema-1" : "schema-1",
                (unsigned int)storedVersion,
                (unsigned int)LINUX_DAEMON_RECORD_VERSION,
                memDetail[0] ? memDetail : "no stored memory offset to convert");
        }
        if (outMigratedFromLegacy) *outMigratedFromLegacy = migratedFromLegacy;
    }
    close(dirfd);
    return result;
}

bool linux_daemon_state_store(const char* path, const LinuxDaemonStateRecord* record,
                              char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!linux_daemon_record_valid(record)) {
        gc_strlcpy(err, errSize, "refusing invalid daemon state record");
        return false;
    }
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    char temp[320] = {};
    bool stored = false;
    for (unsigned int attempt = 0; attempt < 32 && !stored; ++attempt) {
        gc_snprintf(temp, sizeof(temp), ".%s.tmp.%ld.%u", name, (long)getpid(), attempt);
        int fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            gc_snprintf(err, errSize, "cannot create daemon state temp: %s", strerror(errno));
            break;
        }
        bool ok = fchmod(fd, 0600) == 0 && fchown(fd, 0, 0) == 0 &&
                  write_all(fd, record, sizeof(*record)) && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok && renameat(dirfd, temp, dirfd, name) == 0 && fsync(dirfd) == 0) {
            stored = true;
        } else {
            if (err && errSize && !err[0])
                gc_snprintf(err, errSize, "cannot commit daemon state: %s", strerror(errno));
            unlinkat(dirfd, temp, 0);
        }
    }
    close(dirfd);
    return stored;
}

bool linux_daemon_state_remove(const char* path, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    bool ok = unlinkat(dirfd, name, 0) == 0 || errno == ENOENT;
    if (ok) ok = fsync(dirfd) == 0;
    if (!ok) gc_snprintf(err, errSize, "cannot remove daemon state: %s", strerror(errno));
    close(dirfd);
    return ok;
}

bool linux_daemon_operation_store(const char* path,
                                  const LinuxDaemonOperationRecord* record,
                                  char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!linux_daemon_operation_valid(record)) {
        gc_strlcpy(err, errSize, "refusing invalid daemon operation record");
        return false;
    }
    return store_record_atomic(path, record, sizeof(*record), "operation",
                               err, errSize);
}

bool linux_daemon_startup_store(const char* path,
                                const LinuxDaemonStartupRecord* record,
                                char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!linux_daemon_startup_valid(record)) {
        gc_strlcpy(err, errSize, "refusing invalid daemon startup policy record");
        return false;
    }
    return store_record_atomic(path, record, sizeof(*record), "startup policy",
                               err, errSize);
}

bool linux_daemon_startup_load(const char* path,
                               LinuxDaemonStartupRecord* record,
                               bool* outCorrupt, char* err, size_t errSize,
                               bool* outMigratedFromLegacy) {
    if (err && errSize) err[0] = 0;
    if (outCorrupt) *outCorrupt = false;
    if (outMigratedFromLegacy) *outMigratedFromLegacy = false;
    if (record) memset(record, 0, sizeof(*record));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) {
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        // Absent is the documented default (RESTORE_LAST), not a failure.
        if (saved == ENOENT) return false;
        gc_snprintf(err, errSize, "cannot open daemon startup policy: %s",
                    strerror(saved));
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    LinuxDaemonStartupRecord loaded = {};
    struct stat status = {};
    bool protectedRegular = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0077) == 0;
    // Same rule as the state record: size selects the layout, the version
    // inside it selects the value migrations.  A 0.25.2 startup policy is 128
    // bytes shorter than the current one, and demanding the current size is
    // what silently downgraded every upgraded install to "apply nothing at
    // boot" -- fail-safe, but the administrator was never told.
    bool parsed = false;
    bool migrated = false;
    int migratedOldMemMHz = 0;
    gc_u32 storedVersion = 0;
    if (protectedRegular && status.st_size == (off_t)sizeof(loaded)) {
        parsed = read(fd, &loaded, sizeof(loaded)) == (ssize_t)sizeof(loaded);
        if (parsed) storedVersion = loaded.version;
    } else if (protectedRegular &&
               status.st_size == (off_t)sizeof(LinuxDaemonStartupRecordSchema1)) {
        LinuxDaemonStartupRecordSchema1 legacy = {};
        if (read(fd, &legacy, sizeof(legacy)) == (ssize_t)sizeof(legacy) &&
            linux_daemon_startup_widen_schema1(&loaded, &legacy,
                                               &migratedOldMemMHz,
                                               &storedVersion)) {
            parsed = true;
            migrated = true;
        }
    }
    close(fd);
    close(dirfd);
    if (!parsed || !linux_daemon_startup_valid(&loaded)) {
        if (!protectedRegular) {
            gc_snprintf(err, errSize,
                "daemon startup policy rejected: not a root-owned private "
                "regular file (regular=%d uid=%u links=%u mode=%03o)",
                S_ISREG(status.st_mode) ? 1 : 0, (unsigned int)status.st_uid,
                (unsigned int)status.st_nlink,
                (unsigned int)(status.st_mode & 07777));
        } else if (!parsed) {
            gc_snprintf(err, errSize,
                "daemon startup policy rejected: %lld bytes matches no known "
                "record layout (current=%zu schema1=%zu)",
                (long long)status.st_size, sizeof(LinuxDaemonStartupRecord),
                sizeof(LinuxDaemonStartupRecordSchema1));
        } else {
            gc_snprintf(err, errSize,
                "daemon startup policy rejected: record failed validation "
                "(version=%u expected=%u mode=%u slot=%u)",
                (unsigned int)loaded.version,
                (unsigned int)LINUX_DAEMON_STARTUP_VERSION,
                (unsigned int)loaded.mode, (unsigned int)loaded.profileSlot);
        }
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    if (migrated) {
        char memDetail[64] = {};
        if (migratedOldMemMHz != 0)
            gc_snprintf(memDetail, sizeof(memDetail),
                        "mem offset %d -> %d display MHz",
                        migratedOldMemMHz, loaded.desired.memOffsetMHz);
        gc_snprintf(err, errSize,
            "startup policy record migrated from %zu-byte schema-1 v%u to v%u; %s",
            sizeof(LinuxDaemonStartupRecordSchema1), (unsigned int)storedVersion,
            (unsigned int)LINUX_DAEMON_STARTUP_VERSION,
            memDetail[0] ? memDetail : "no stored memory offset to convert");
        if (outMigratedFromLegacy) *outMigratedFromLegacy = true;
    }
    if (record) *record = loaded;
    return true;
}

bool linux_read_boot_id(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char raw[64] = {};
    ssize_t count = -1;
    do {
        count = read(fd, raw, sizeof(raw) - 1);
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0) return false;
    raw[count] = 0;
    // The kernel appends a newline; a boot identity with a stray terminator in
    // it would compare unequal to itself on the next start.
    size_t length = 0;
    while (raw[length] && raw[length] != '\n' && raw[length] != '\r') ++length;
    if (length == 0 || length >= outSize) return false;
    memcpy(out, raw, length);
    out[length] = 0;
    return true;
}

bool linux_daemon_guard_store(const char* path,
                              const LinuxAutoRestoreGuard* guard,
                              char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!guard) {
        gc_strlcpy(err, errSize, "refusing to store a null automatic-restore guard");
        return false;
    }
    LinuxDaemonRestoreGuardRecord record = {};
    linux_daemon_guard_initialize(&record, guard);
    if (!linux_daemon_guard_valid(&record)) {
        gc_strlcpy(err, errSize, "refusing invalid automatic-restore guard record");
        return false;
    }
    return store_record_atomic(path, &record, sizeof(record),
                               "automatic-restore guard", err, errSize);
}

bool linux_daemon_guard_load(const char* path, LinuxAutoRestoreGuard* guard,
                             bool* outCorrupt, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (outCorrupt) *outCorrupt = false;
    if (guard) memset(guard, 0, sizeof(*guard));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) {
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        // Absent is the normal first-run state, not a fault.
        if (saved == ENOENT) return false;
        gc_snprintf(err, errSize, "cannot open automatic-restore guard: %s",
                    strerror(saved));
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    LinuxDaemonRestoreGuardRecord loaded = {};
    struct stat status = {};
    bool ok = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0077) == 0 &&
        status.st_size == (off_t)sizeof(loaded) &&
        read(fd, &loaded, sizeof(loaded)) == (ssize_t)sizeof(loaded) &&
        linux_daemon_guard_valid(&loaded);
    close(fd);
    close(dirfd);
    if (!ok) {
        gc_strlcpy(err, errSize, "automatic-restore guard record is invalid");
        if (outCorrupt) *outCorrupt = true;
        return false;
    }
    if (guard) linux_daemon_guard_to_policy(&loaded, guard);
    return true;
}

bool linux_daemon_operation_load(const char* path,
                                 LinuxDaemonOperationRecord* record,
                                 char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (record) memset(record, 0, sizeof(*record));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return false;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        if (saved != ENOENT)
            gc_snprintf(err, errSize, "cannot open daemon operation: %s",
                strerror(saved));
        return false;
    }
    LinuxDaemonOperationRecord loaded = {};
    struct stat status = {};
    bool ok = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0077) == 0 &&
        read(fd, &loaded, sizeof(loaded)) == (ssize_t)sizeof(loaded) &&
        linux_daemon_operation_valid(&loaded);
    close(fd);
    close(dirfd);
    if (!ok) {
        gc_strlcpy(err, errSize, "daemon operation record is invalid");
        return false;
    }
    if (record) *record = loaded;
    return true;
}

bool linux_fan_ownership_marker_store(const char* path,
                                      const LinuxFanOwnershipMarker* marker,
                                      char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!linux_fan_ownership_marker_valid(marker)) {
        gc_strlcpy(err, errSize, "refusing invalid fan ownership marker");
        return false;
    }
    return store_record_atomic(path, marker, sizeof(*marker),
                               "fan ownership marker", err, errSize);
}

int linux_fan_ownership_marker_load(const char* path,
                                    LinuxFanOwnershipMarker* marker,
                                    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (marker) memset(marker, 0, sizeof(*marker));
    char name[256] = {};
    int dirfd = open_state_directory(path, name, sizeof(name), err, errSize);
    if (dirfd < 0) return -1;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        int saved = errno;
        close(dirfd);
        if (saved == ENOENT) return 0;
        gc_snprintf(err, errSize, "cannot open fan ownership marker: %s",
                    strerror(saved));
        return -1;
    }
    LinuxFanOwnershipMarker loaded = {};
    struct stat status = {};
    bool ok = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0077) == 0 &&
        status.st_size == (off_t)sizeof(loaded) &&
        read(fd, &loaded, sizeof(loaded)) == (ssize_t)sizeof(loaded) &&
        linux_fan_ownership_marker_valid(&loaded);
    close(fd);
    close(dirfd);
    if (!ok) {
        gc_strlcpy(err, errSize, "fan ownership marker is invalid");
        return -1;
    }
    if (marker) *marker = loaded;
    return 1;
}
