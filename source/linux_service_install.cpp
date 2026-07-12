// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp; do not compile separately.

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

int linux_service_install(char* err, size_t errSize) {
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

    // Admin group for socket access (best-effort; ignore "already exists").
    char* groupArgs[] = {(char*)"groupadd", (char*)"-f", (char*)"greencurve", nullptr};
    int groupResult = run_root_command("/usr/sbin/groupadd", groupArgs);
    if (groupResult == 127)
        groupResult = run_root_command("/sbin/groupadd", groupArgs);
    if (groupResult != 0)
        dlog("service-install: groupadd greencurve failed (non-fatal)\n");

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
        "Type=simple\n"
        "ExecStart=%s --daemon\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "StateDirectory=greencurve\n"
        "RuntimeDirectory=greencurve\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        GC_INSTALL_BIN);
    bool unitOk = unitWritten > 0 && fflush(f) == 0 && fsync(unitFd) == 0;
    if (fclose(f) != 0) unitOk = false;
    if (!unitOk) { gc_strlcpy(err, errSize, "failed to commit systemd unit"); return 1; }

    char* reloadArgs[] = {(char*)"systemctl", (char*)"daemon-reload", nullptr};
    if (run_systemctl(reloadArgs) != 0)
        dlog("service-install: systemctl daemon-reload failed (non-fatal)\n");
    char* enableArgs[] = {(char*)"systemctl", (char*)"enable", (char*)"--now",
                          (char*)"greencurve.service", nullptr};
    if (run_systemctl(enableArgs) != 0) {
        gc_strlcpy(err, errSize,
                   "systemctl enable --now greencurve.service failed "
                   "(check: journalctl -u greencurve)");
        return 1;
    }
    return 0;
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
