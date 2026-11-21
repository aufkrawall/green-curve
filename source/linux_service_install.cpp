// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

#include "linux_service_install_policy.h"
#include "linux_service_install_files.h"
#include "linux_service_sandbox.h"
#include "linux_socket_path_permissions.h"

// ===========================================================================
// systemd install / remove  (requires root)
// ===========================================================================
#define GC_UNIT_PATH "/etc/systemd/system/greencurve.service"
#define GC_RESUME_UNIT_NAME "greencurve-resume.service"
#define GC_RESUME_UNIT_PATH "/etc/systemd/system/" GC_RESUME_UNIT_NAME
#define GC_INSTALL_DIR "/usr/local/libexec/greencurve"
#define GC_INSTALL_BIN GC_INSTALL_DIR "/greencurve"
#define GC_SYMLINK_BIN "/usr/local/bin/greencurve"

static bool root_owned_nonwritable_path(const char* path, bool wantDir, char* err, size_t errSize) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        gc_snprintf(err, errSize, "cannot inspect %s: %s", path, strerror(errno));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is a symlink", path);
        return false;
    }
    if (wantDir && !S_ISDIR(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is not a directory", path);
        return false;
    }
    if (!wantDir && !S_ISREG(st.st_mode)) {
        gc_snprintf(err, errSize, "%s is not a regular file", path);
        return false;
    }
    if (st.st_uid != 0) {
        gc_snprintf(err, errSize, "%s is not root-owned", path);
        return false;
    }
    if ((st.st_mode & 0022) != 0) {
        gc_snprintf(err, errSize, "%s is writable by group/other", path);
        return false;
    }
    return true;
}

static bool ensure_root_owned_dir(const char* path, mode_t mode, char* err, size_t errSize) {
    if (mkdir(path, mode) != 0) {
        if (errno == EEXIST) {
            return root_owned_nonwritable_path(path, true, err, errSize);
        }
        gc_snprintf(err, errSize, "cannot create %s: %s", path, strerror(errno));
        return false;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        gc_snprintf(err, errSize, "cannot safely open %s: %s", path, strerror(errno));
        return false;
    }
    if (fchown(fd, 0, 0) != 0) {
        gc_snprintf(err, errSize, "cannot chown %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    if (fchmod(fd, mode) != 0) {
        gc_snprintf(err, errSize, "cannot chmod %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);
    return root_owned_nonwritable_path(path, true, err, errSize);
}

static bool validate_install_parent_chain(char* err, size_t errSize) {
    if (!root_owned_nonwritable_path("/usr", true, err, errSize)) return false;
    if (!root_owned_nonwritable_path("/usr/local", true, err, errSize)) return false;
    if (!ensure_root_owned_dir("/usr/local/libexec", 0755, err, errSize)) return false;
    if (!ensure_root_owned_dir(GC_INSTALL_DIR, 0755, err, errSize)) return false;
    return true;
}

static bool write_all_file(int fd, const void* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool stage_service_binary(LinuxInstallFile& file, const char* sourceExe,
                                 char* err, size_t errSize) {
    int in = open(sourceExe, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        gc_snprintf(err, errSize, "cannot open source executable %s: %s", sourceExe, strerror(errno));
        return false;
    }
    int out = file.stage(0755);
    if (out < 0) {
        gc_snprintf(err, errSize, "cannot create staged executable %s: %s", file.path(), strerror(errno));
        close(in);
        return false;
    }

    bool ok = true;
    unsigned char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n > 0) {
            if (!write_all_file(out, buf, (size_t)n)) { ok = false; break; }
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        ok = false;
        break;
    }
    if (fsync(out) != 0) ok = false;
    if (close(out) != 0) ok = false;
    close(in);
    if (!ok) {
        gc_snprintf(err, errSize, "failed staging executable %s", file.path());
    }
    return ok;
}

static int run_root_command(const char* path, char* const argv[]) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        // flawfinder: ignore -- fixed root-owned absolute path and fixed argv; no shell.
        execv(path, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_systemctl(char* const argv[]) {
    int result = run_root_command("/usr/bin/systemctl", argv);
    if (result != 127) return result;
    return run_root_command("/bin/systemctl", argv);
}

struct LinuxServiceActivationContext {
    ServiceResponse verifiedResponse;
    bool wasEnabled;
    bool resumeWasEnabled;
};

static bool run_service_activation_step(
    void* opaque, LinuxServiceActivationStep step,
    char* err, size_t errSize) {
    LinuxServiceActivationContext* context =
        (LinuxServiceActivationContext*)opaque;
    int commandResult = -1;
    switch (step) {
        case LINUX_SERVICE_STEP_DAEMON_RELOAD: {
            char* args[] = {(char*)"systemctl", (char*)"daemon-reload", nullptr};
            commandResult = run_systemctl(args);
            break;
        }
        case LINUX_SERVICE_STEP_ENABLE: {
            if (context->wasEnabled) return true;
            char* args[] = {(char*)"systemctl", (char*)"enable",
                            (char*)"greencurve.service", nullptr};
            commandResult = run_systemctl(args);
            break;
        }
        case LINUX_SERVICE_STEP_ENABLE_RESUME: {
            if (context->resumeWasEnabled) return true;
            char* args[] = {(char*)"systemctl", (char*)"enable",
                            (char*)GC_RESUME_UNIT_NAME, nullptr};
            commandResult = run_systemctl(args);
            break;
        }
        case LINUX_SERVICE_STEP_RESTART: {
            // Deliberately unconditional: `enable --now` leaves an already
            // running old executable/protocol resident after an upgrade.
            char* args[] = {(char*)"systemctl", (char*)"restart",
                            (char*)"greencurve.service", nullptr};
            commandResult = run_systemctl(args);
            break;
        }
        case LINUX_SERVICE_STEP_IS_ACTIVE: {
            char* args[] = {(char*)"systemctl", (char*)"is-active",
                            (char*)"--quiet", (char*)"greencurve.service", nullptr};
            commandResult = run_systemctl(args);
            break;
        }
        case LINUX_SERVICE_STEP_VERIFY_SOCKET: {
            struct group* serviceGroup = getgrnam("greencurve");
            if (!serviceGroup) {
                gc_strlcpy(err, errSize,
                    "greencurve group disappeared before socket verification");
                return false;
            }
            int directoryFd = open(GC_DAEMON_SOCKET_DIR,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (directoryFd < 0) {
                gc_snprintf(err, errSize,
                    "cannot open daemon socket directory %s: %s",
                    GC_DAEMON_SOCKET_DIR, strerror(errno));
                return false;
            }
            struct stat directoryStatus = {};
            if (fstat(directoryFd, &directoryStatus) != 0) {
                gc_snprintf(err, errSize,
                    "cannot inspect daemon socket directory %s: %s",
                    GC_DAEMON_SOCKET_DIR, strerror(errno));
                close(directoryFd);
                return false;
            }
            bool directoryValid = S_ISDIR(directoryStatus.st_mode) &&
                directoryStatus.st_uid == 0 &&
                (directoryStatus.st_mode & 0777) == 0755;
            if (!directoryValid) {
                gc_snprintf(err, errSize,
                    "daemon socket directory has uid=%lu mode=%04o; expected uid=0 mode=0755",
                    (unsigned long)directoryStatus.st_uid,
                    (unsigned int)(directoryStatus.st_mode & 0777));
                close(directoryFd);
                return false;
            }
            bool verified = linux_verify_socket_path_permissions_at(
                directoryFd, GC_DAEMON_SOCKET_NAME, 0,
                serviceGroup->gr_gid, 0660, err, errSize);
            close(directoryFd);
            if (!verified) return false;
            dlog("service-install: verified socket pathname %s root:greencurve mode=0660\n",
                 GC_DAEMON_SOCKET_PATH);
            return true;
        }
        case LINUX_SERVICE_STEP_VERIFY_PROTOCOL: {
            ServiceRequest request = {};
            request.magic = SERVICE_PROTOCOL_MAGIC;
            request.version = SERVICE_PROTOCOL_VERSION;
            request.command = SERVICE_CMD_PING;
            request.callerPid = (gc_u32)getpid();
            ServiceResponse response = {};
            char transportError[256] = {};
            if (!linux_daemon_send(&request, &response,
                                   transportError, sizeof(transportError))) {
                gc_snprintf(err, errSize,
                    "daemon ping failed after restart: %s",
                    transportError[0] ? transportError : "unknown transport failure");
                return false;
            }
            if (response.version != SERVICE_PROTOCOL_VERSION ||
                response.serviceBuildNumber != (gc_u32)APP_BUILD_NUMBER ||
                strcmp(response.serviceVersion, APP_VERSION) != 0 ||
                response.servicePid == 0) {
                gc_snprintf(err, errSize,
                    "daemon verification mismatch: expected version=%s build=%u protocol=%u; "
                    "received version=%s build=%u protocol=%u pid=%u",
                    APP_VERSION, (unsigned int)APP_BUILD_NUMBER,
                    (unsigned int)SERVICE_PROTOCOL_VERSION,
                    response.serviceVersion, response.serviceBuildNumber,
                    response.version, response.servicePid);
                return false;
            }
            context->verifiedResponse = response;
            dlog("service-install: verified version=%s build=%u protocol=%u pid=%u phase=%u health=%s\n",
                 response.serviceVersion, response.serviceBuildNumber,
                 response.version, response.servicePid,
                 response.state.gpuPhase,
                 service_gpu_health_reason_name(response.snapshot.health.reason));
            return true;
        }
        default:
            gc_strlcpy(err, errSize, "unknown service activation step");
            return false;
    }
    if (commandResult != 0) {
        gc_snprintf(err, errSize, "%s failed with exit code %d",
                    linux_service_activation_step_name(step), commandResult);
        return false;
    }
    return true;
}

// The standby-resume restore.  Windows gets this from PBT_APMRESUMEAUTOMATIC
// inside the service; Linux has no in-process power notification without a
// D-Bus client, so systemd's own sleep ordering supplies the edge instead.
//
// A unit that is BOTH `WantedBy=` and `After=` a sleep target runs on the way
// back UP: systemd walks the ordering in reverse on the way down, so the
// combination below is the documented way to say "after resume", not "before
// suspend".
//
// The readiness question is answered by ordering rather than by waiting.
// nvidia-resume.service is the driver's own "the GPU is usable again" unit, so
// being ordered after it is a real signal and not a timing guess; the
// ConditionPathExists on the socket means a machine whose daemon is not running
// SKIPS this unit instead of failing it once per wake.
static bool stage_resume_unit(LinuxInstallFile& file, char* err, size_t errSize) {
    int fd = file.stage(0644);
    if (fd < 0) {
        gc_snprintf(err, errSize, "cannot stage %s: %s", file.path(), strerror(errno));
        return false;
    }
    FILE* stream = fdopen(fd, "w");
    if (!stream) {
        close(fd);
        gc_strlcpy(err, errSize, "cannot open resume unit stream");
        return false;
    }
    int written = fprintf(stream,
        "[Unit]\n"
        "Description=Green Curve settings restore after suspend\n"
        "Documentation=https://github.com/aufkrawall/greencurve\n"
        "After=suspend.target hibernate.target hybrid-sleep.target"
        " suspend-then-hibernate.target\n"
        "After=nvidia-resume.service\n"
        "After=greencurve.service\n"
        "ConditionPathExists=%s\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=%s --resume-restore\n"
        "StandardOutput=journal\n"
        "StandardError=journal\n"
        GC_LINUX_SERVICE_SANDBOX
        "\n"
        "[Install]\n"
        "WantedBy=suspend.target hibernate.target hybrid-sleep.target"
        " suspend-then-hibernate.target\n",
        GC_DAEMON_SOCKET_PATH, GC_INSTALL_BIN);
    bool ok = written > 0 && fflush(stream) == 0 && fsync(fd) == 0;
    if (fclose(stream) != 0) ok = false;
    if (!ok) {
        gc_strlcpy(err, errSize, "failed to commit the resume systemd unit");
        return false;
    }
    dlog("service-install: staged %s (resume restore after suspend)\n",
         GC_RESUME_UNIT_PATH);
    return true;
}

static bool stage_daemon_unit(LinuxInstallFile& file, char* err, size_t errSize) {
    int unitFd = file.stage(0644);
    if (unitFd < 0) {
        gc_snprintf(err, errSize, "cannot stage %s: %s", file.path(), strerror(errno));
        return false;
    }
    FILE* f = fdopen(unitFd, "w");
    if (!f) {
        close(unitFd);
        gc_strlcpy(err, errSize, "cannot open systemd unit stream");
        return false;
    }
    int unitWritten = fprintf(f,
        "[Unit]\n"
        "Description=Green Curve NVIDIA GPU control daemon\n"
        // Deliberately NOT `After=multi-user.target`.  That ordering was
        // self-defeating: the unit is WantedBy that same target, so ordering
        // after it scheduled Green Curve dead last in boot, behind every other
        // service that might touch the GPU.  Ordering against the driver's own
        // units is the real dependency, and it is ordering only -- no Wants=,
        // so a machine without nvidia-persistenced is unaffected rather than
        // having a service started for it.
        "After=nvidia-persistenced.service\n"
        // Windows' SCM resets its failure count after ten minutes and re-arms
        // the restart actions. systemd's start limit is a rolling window with a
        // 5-in-10s default, which a service that crashes on a two-second
        // RestartSec= trips almost immediately -- and once tripped the unit
        // stays failed until someone runs `systemctl reset-failed`. A ten
        // minute window gives the same shape as the SCM: spaced-out driver
        // events each get a fresh restart, a tight crash loop still stops.
        "StartLimitIntervalSec=600\n"
        "StartLimitBurst=5\n\n"
        "[Service]\n"
        "Type=notify\n"
        "NotifyAccess=main\n"
        "ExecStart=%s --daemon\n"
        // `always`, not `on-failure`. The daemon exits 0 for any stop signal,
        // so an out-of-band `kill -TERM` left the machine with no GPU control
        // and systemd declining to act -- while the same thing on Windows
        // (TerminateProcess) is exactly what the SCM restart actions cover.
        // `systemctl stop` still wins: systemd never restarts a unit it stopped.
        "Restart=always\n"
        "RestartSec=2\n"
        // Liveness, not a poll: the daemon pings from its accept loop, so a
        // wedged serve loop is restarted instead of sitting there `active`
        // while nothing re-asserts the fan. Generous on purpose -- a VF apply
        // with per-point readback verification is the longest thing that runs
        // between two pings.
        "WatchdogSec=120\n"
        "StateDirectory=greencurve\n"
        "RuntimeDirectory=greencurve\n"
        "RuntimeDirectoryMode=0755\n"
        GC_LINUX_SERVICE_SANDBOX
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        GC_INSTALL_BIN);
    bool unitOk = unitWritten > 0 && fflush(f) == 0 && fsync(unitFd) == 0;
    if (fclose(f) != 0) unitOk = false;
    if (!unitOk) gc_strlcpy(err, errSize, "failed to stage systemd unit");
    return unitOk;
}

static int service_install_unit_command(const char* command, const char* unit = nullptr) {
    char* args[] = {(char*)"systemctl", const_cast<char*>(command),
        const_cast<char*>(unit), nullptr};
    return run_systemctl(args);
}

static bool service_install_unit_state(const char* query, const char* unit, bool* set,
                                       char* err, size_t errSize) {
    const int result = service_install_unit_command(query, unit);
    *set = result == 0;
    if (result == 0 || result == 1 || result == 3 || result == 4) return true;
    gc_snprintf(err, errSize, "systemctl %s %s failed with exit code %d", query, unit, result);
    return false;
}

static bool service_install_restore_files(LinuxInstallFile* const* files, size_t count) {
    bool restored = true;
    while (count > 0) {
        LinuxInstallFile& file = *files[--count];
        if (file.restore()) continue;
        dlog("service-install: rollback of %s failed (%s), backup=%s\n",
             file.path(), strerror(errno), file.backup());
        restored = false;
    }
    return restored;
}

int linux_service_install(char* err, size_t errSize,
                          ServiceResponse* verifiedResponse) {
    if (err && errSize) err[0] = 0;
    if (geteuid() != 0) {
        gc_strlcpy(err, errSize, "--service-install requires root (use sudo)");
        return 1;
    }
    char exe[4096] = {};
    // flawfinder: ignore -- kernel-owned /proc/self/exe, bounded and NUL-terminated below.
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { gc_strlcpy(err, errSize, "cannot resolve /proc/self/exe"); return 1; }
    exe[n] = 0;
    if (!validate_install_parent_chain(err, errSize) ||
        !root_owned_nonwritable_path("/etc", true, err, errSize) ||
        !root_owned_nonwritable_path("/etc/systemd", true, err, errSize) ||
        !root_owned_nonwritable_path("/etc/systemd/system", true, err, errSize)) return 1;
    LinuxServiceInstallLock installLock(GC_INSTALL_DIR);
    if (!installLock.locked()) {
        gc_snprintf(err, errSize, "cannot lock daemon installation: %s", strerror(errno));
        return 1;
    }

    fprintf(stderr, "The greencurve group grants trusted GPU administration, including persistent boot settings. Removing membership does not clear saved boot settings; clear those separately.\n");
    // Admin group creation and verification are part of the authorization
    // boundary. Installation must not claim success without them.
    char* groupArgs[] = {(char*)"groupadd", (char*)"-f", (char*)"greencurve", nullptr};
    int groupResult = run_root_command("/usr/sbin/groupadd", groupArgs);
    if (groupResult == 127)
        groupResult = run_root_command("/sbin/groupadd", groupArgs);
    if (groupResult != 0) {
        gc_snprintf(err, errSize,
            "groupadd -f greencurve failed with exit code %d", groupResult);
        return 1;
    }
    struct group* serviceGroup = getgrnam("greencurve");
    if (!serviceGroup || !serviceGroup->gr_name ||
        strcmp(serviceGroup->gr_name, "greencurve") != 0) {
        gc_strlcpy(err, errSize,
            "greencurve group verification failed after groupadd");
        return 1;
    }

    LinuxInstallFile binary(GC_INSTALL_BIN);
    LinuxInstallFile unit(GC_UNIT_PATH);
    LinuxInstallFile resume(GC_RESUME_UNIT_PATH);
    LinuxInstallFile* const files[] = {&binary, &unit, &resume};
    if (!stage_service_binary(binary, exe, err, errSize) ||
        !stage_daemon_unit(unit, err, errSize) ||
        !stage_resume_unit(resume, err, errSize)) return 1;

    bool wasActive = false, wasEnabled = false, resumeWasEnabled = false;
    if (!service_install_unit_state("is-active", "greencurve.service", &wasActive, err, errSize) ||
        !service_install_unit_state("is-enabled", "greencurve.service", &wasEnabled, err, errSize) ||
        !service_install_unit_state("is-enabled", GC_RESUME_UNIT_NAME, &resumeWasEnabled, err, errSize))
        return 1;
    for (auto* file : files) {
        if (!file->save_previous()) {
            gc_snprintf(err, errSize, "cannot preserve %s: %s", file->path(), strerror(errno));
            return 1;
        }
    }
    for (auto* file : files) {
        if (file->publish()) continue;
        const int error = errno;
        const bool restored = service_install_restore_files(files, sizeof(files) / sizeof(files[0]));
        gc_snprintf(err, errSize, "cannot publish %s: %s; file rollback %s",
            file->path(), strerror(error), restored ? "completed" : "incomplete");
        return 1;
    }

    LinuxServiceActivationContext activation = {{}, wasEnabled, resumeWasEnabled};
    const LinuxServiceActivationResult activationResult =
        linux_service_run_activation(run_service_activation_step, &activation, err, errSize);
    if (!activationResult.success) {
        char activationError[512] = {};
        gc_strlcpy(activationError, sizeof(activationError), err && err[0]
            ? err : linux_service_activation_step_name(activationResult.failedStep));
        const auto step = activationResult.failedStep;
        bool restored = true;
        if (step >= LINUX_SERVICE_STEP_RESTART)
            restored &= service_install_unit_command("stop", "greencurve.service") == 0;
        if (step >= LINUX_SERVICE_STEP_ENABLE_RESUME && !resumeWasEnabled)
            restored &= service_install_unit_command("disable", GC_RESUME_UNIT_NAME) == 0;
        if (step >= LINUX_SERVICE_STEP_ENABLE && !wasEnabled)
            restored &= service_install_unit_command("disable", "greencurve.service") == 0;
        const bool filesRestored = service_install_restore_files(files, sizeof(files) / sizeof(files[0]));
        const bool reloaded = service_install_unit_command("daemon-reload") == 0;
        restored &= filesRestored && reloaded;
        if (step >= LINUX_SERVICE_STEP_RESTART && wasActive && filesRestored && reloaded)
            restored &= service_install_unit_command("start", "greencurve.service") == 0;
        dlog("service-install: activation failed at %s; rollback=%d oldDaemonActive=%d\n",
            linux_service_activation_step_name(step), restored ? 1 : 0, wasActive ? 1 : 0);
        gc_snprintf(err, errSize, "%s; installation rollback %s",
            activationError, restored ? "completed" : "incomplete");
        return 1;
    }
    for (auto* file : files) {
        if (!file->commit())
            dlog("service-install: backup cleanup failed for %s: %s\n", file->path(), strerror(errno));
    }
    if (verifiedResponse) *verifiedResponse = activation.verifiedResponse;

    unlink(GC_SYMLINK_BIN);
    if (symlink(GC_INSTALL_BIN, GC_SYMLINK_BIN) != 0) {
        dlog("service-install: symlink %s -> %s failed: %s (non-fatal)\n",
             GC_SYMLINK_BIN, GC_INSTALL_BIN, strerror(errno));
    } else {
        dlog("service-install: symlinked %s -> %s\n",
             GC_SYMLINK_BIN, GC_INSTALL_BIN);
    }
    return 0;
}

// Resolves the account the install was performed *for* and reports the group
// paragraph to print.  Root itself is never the answer: it does not need the
// group, and naming it would print a command that changes nothing.
//
// SUDO_USER is authoritative when present.  getlogin() covers the plain root
// console / `su -` case, which is precisely where greencurve-setup.sh also
// cannot resolve an account, so both agree about when to fall back.
void linux_describe_group_enrollment(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';

    // greencurve-setup.sh enrolls the account itself, but only *after* this
    // step, because the group does not exist until --service-install creates
    // it.  Without this the summary would prescribe usermod on the line right
    // before the script ran it.  Whoever performs the enrollment owns the
    // message about it; the script sets this when it has resolved an account.
    const char* deferred = getenv("GREENCURVE_SETUP_OWNS_GROUP");
    if (deferred && deferred[0] == '1' && deferred[1] == '\0') {
        dlog("service-install: group advice suppressed; the setup wrapper "
             "owns enrollment for this run\n");
        return;
    }

    char account[256] = {};
    const char* sudoUser = getenv("SUDO_USER");
    if (sudoUser && sudoUser[0] && strcmp(sudoUser, "root") != 0) {
        gc_strlcpy(account, sizeof(account), sudoUser);
    } else {
        const char* login = getlogin();
        if (login && login[0] && strcmp(login, "root") != 0)
            gc_strlcpy(account, sizeof(account), login);
    }

    struct group* serviceGroup = getgrnam("greencurve");
    bool groupExists = serviceGroup && serviceGroup->gr_name;
    bool member = false;
    if (groupExists && account[0]) {
        for (char** m = serviceGroup->gr_mem; m && *m; ++m) {
            if (strcmp(*m, account) == 0) { member = true; break; }
        }
        // A user whose *primary* group is greencurve is a member without
        // appearing in gr_mem at all.
        if (!member) {
            struct passwd* pw = getpwnam(account);
            if (pw && pw->pw_gid == serviceGroup->gr_gid) member = true;
        }
    }

    dlog("service-install: group advice account='%s' groupExists=%d member=%d\n",
         account[0] ? account : "(unresolved)", (int)groupExists, (int)member);
    linux_format_group_enrollment_advice(
        linux_group_enrollment_advice(account, groupExists, member),
        account, out, outSize);
}

int linux_service_remove(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (geteuid() != 0) {
        gc_strlcpy(err, errSize, "--service-remove requires root (use sudo)");
        return 1;
    }
    // The resume unit goes first: leaving it enabled after the daemon unit is
    // gone would leave a unit wanted by every sleep target pointing at a binary
    // that is about to be removed.
    char* disableResumeArgs[] = {(char*)"systemctl", (char*)"disable",
                                 (char*)GC_RESUME_UNIT_NAME, nullptr};
    if (run_systemctl(disableResumeArgs) != 0)
        dlog("service-remove: resume unit disable failed (non-fatal)\n");
    unlink(GC_RESUME_UNIT_PATH);
    char* disableArgs[] = {(char*)"systemctl", (char*)"disable", (char*)"--now",
                           (char*)"greencurve.service", nullptr};
    if (run_systemctl(disableArgs) != 0)
        dlog("service-remove: disable failed (non-fatal)\n");
    unlink(GC_UNIT_PATH);
    unlink(GC_SYMLINK_BIN);
    char* reloadArgs[] = {(char*)"systemctl", (char*)"daemon-reload", nullptr};
    if (run_systemctl(reloadArgs) != 0)
        dlog("service-remove: daemon-reload failed (non-fatal)\n");
    // A unit that spent its start-limit budget stays `failed` until someone
    // resets it, and a removal is exactly the point at which that bookkeeping
    // stops being useful. Non-fatal: a unit that was never failed makes this a
    // no-op, and neither outcome should fail the uninstall.
    char* resetArgs[] = {(char*)"systemctl", (char*)"reset-failed",
                         (char*)"greencurve.service",
                         (char*)GC_RESUME_UNIT_NAME, nullptr};
    if (run_systemctl(resetArgs) != 0)
        dlog("service-remove: reset-failed failed (non-fatal)\n");
    return 0;
}
