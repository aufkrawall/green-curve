// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

#include "linux_service_install_policy.h"
#include "linux_socket_path_permissions.h"

// ===========================================================================
// systemd install / remove  (requires root)
// ===========================================================================
#define GC_UNIT_PATH "/etc/systemd/system/greencurve.service"
#define GC_INSTALL_DIR "/usr/local/libexec/greencurve"
#define GC_INSTALL_BIN GC_INSTALL_DIR "/greencurve"

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

static bool stage_service_binary(const char* sourceExe, char* err, size_t errSize) {
    if (!validate_install_parent_chain(err, errSize)) return false;

    char tempPath[4096] = {};
    gc_snprintf(tempPath, sizeof(tempPath), "%s.tmp.%ld", GC_INSTALL_BIN, (long)getpid());
    unlink(tempPath);

    int in = open(sourceExe, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        gc_snprintf(err, errSize, "cannot open source executable %s: %s", sourceExe, strerror(errno));
        return false;
    }
    int out = open(tempPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
    if (out < 0) {
        gc_snprintf(err, errSize, "cannot create staged executable %s: %s", tempPath, strerror(errno));
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
    if (fchown(out, 0, 0) != 0) ok = false;
    if (fchmod(out, 0755) != 0) ok = false;
    if (close(out) != 0) ok = false;
    close(in);
    if (!ok) {
        gc_snprintf(err, errSize, "failed staging executable %s: %s", tempPath, strerror(errno));
        unlink(tempPath);
        return false;
    }
    if (rename(tempPath, GC_INSTALL_BIN) != 0) {
        gc_snprintf(err, errSize, "cannot install %s: %s", GC_INSTALL_BIN, strerror(errno));
        unlink(tempPath);
        return false;
    }
    if (!root_owned_nonwritable_path(GC_INSTALL_BIN, false, err, errSize)) return false;
    int dirfd = open(GC_INSTALL_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }
    return validate_install_parent_chain(err, errSize);
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
            char* args[] = {(char*)"systemctl", (char*)"enable",
                            (char*)"greencurve.service", nullptr};
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
    if (!stage_service_binary(exe, err, errSize)) return 1;

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

    int unitFd = open(GC_UNIT_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    struct stat unitStat = {};
    if (unitFd < 0 || fstat(unitFd, &unitStat) != 0 ||
        !S_ISREG(unitStat.st_mode) || unitStat.st_nlink != 1 ||
        fchown(unitFd, 0, 0) != 0 || fchmod(unitFd, 0644) != 0) {
        gc_snprintf(err, errSize, "cannot safely write %s: %s", GC_UNIT_PATH, strerror(errno));
        if (unitFd >= 0) close(unitFd);
        return 1;
    }
    FILE* f = fdopen(unitFd, "w");
    if (!f) { close(unitFd); gc_strlcpy(err, errSize, "cannot open systemd unit stream"); return 1; }
    int unitWritten = fprintf(f,
        "[Unit]\n"
        "Description=Green Curve NVIDIA GPU control daemon\n"
        "After=multi-user.target\n\n"
        "[Service]\n"
        "Type=notify\n"
        "NotifyAccess=main\n"
        "ExecStart=%s --daemon\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "UMask=0077\n"
        "NoNewPrivileges=true\n"
        "StateDirectory=greencurve\n"
        "RuntimeDirectory=greencurve\n"
        "RuntimeDirectoryMode=0755\n"
        // Sandboxing.  The daemon needs root, the NVIDIA character devices, and
        // its own state/runtime directories -- nothing else.  ProtectSystem is
        // deliberately "full" rather than "strict": the NVIDIA user-mode stack
        // resolves libraries and driver state under /usr and /sys at runtime,
        // and StateDirectory/RuntimeDirectory already remain writable.
        "ProtectSystem=full\n"
        "ProtectHome=yes\n"
        "PrivateTmp=yes\n"
        "ProtectControlGroups=yes\n"
        "ProtectKernelLogs=yes\n"
        "RestrictSUIDSGID=yes\n"
        "RestrictNamespaces=yes\n"
        "RestrictRealtime=yes\n"
        // The control socket is AF_UNIX only; the daemon has no network path.
        "RestrictAddressFamilies=AF_UNIX\n"
        "SystemCallArchitectures=native\n"
        // Deliberately not set: MemoryDenyWriteExecute (the NVIDIA user-mode
        // stack maps writable-executable pages), ProtectKernelTunables and
        // ProtectKernelModules (driver state lives under /proc/driver/nvidia),
        // and PrivateDevices (the daemon needs /dev/nvidia*).
        "LockPersonality=yes\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        GC_INSTALL_BIN);
    bool unitOk = unitWritten > 0 && fflush(f) == 0 && fsync(unitFd) == 0;
    if (fclose(f) != 0) unitOk = false;
    if (!unitOk) { gc_strlcpy(err, errSize, "failed to commit systemd unit"); return 1; }

    LinuxServiceActivationContext activation = {};
    LinuxServiceActivationResult activationResult =
        linux_service_run_activation(run_service_activation_step,
                                     &activation, err, errSize);
    if (!activationResult.success) {
        if (err && errSize && !err[0]) {
            gc_snprintf(err, errSize, "%s failed",
                linux_service_activation_step_name(activationResult.failedStep));
        }
        return 1;
    }
    if (verifiedResponse) *verifiedResponse = activation.verifiedResponse;
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
    char* disableArgs[] = {(char*)"systemctl", (char*)"disable", (char*)"--now",
                           (char*)"greencurve.service", nullptr};
    if (run_systemctl(disableArgs) != 0)
        dlog("service-remove: disable failed (non-fatal)\n");
    unlink(GC_UNIT_PATH);
    char* reloadArgs[] = {(char*)"systemctl", (char*)"daemon-reload", nullptr};
    if (run_systemctl(reloadArgs) != 0)
        dlog("service-remove: daemon-reload failed (non-fatal)\n");
    return 0;
}
