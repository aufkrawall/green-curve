// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_COMMANDS_LINUX_H
#define GREEN_CURVE_PROFILE_COMMANDS_LINUX_H
#include "profile_commands.h"
#include "profile_legacy_document.h"
#include "profile_file_read.h"
#include "profile_capture_commands.h"
#include "profile_check_commands.h"
#include "linux_config_lock.h"
#include "linux_debug_log.h"
#include "profile_slot_io_linux.h"
#include <stdlib.h>

struct LinuxProfileCommands : LinuxProfileSlotIO {
    bool capture(gc_u32 domains, HardwareCaptureObservation& output, ProfileCodecError& error) {
        ServiceRequest request{}; auto response = std::make_unique<ServiceResponse>();
        request.magic = SERVICE_PROTOCOL_MAGIC; request.version = SERVICE_PROTOCOL_VERSION;
        request.command = SERVICE_CMD_CAPTURE_CURRENT; request.captureDomains = domains;
        request.callerPid = (gc_u32)getpid();
        if (!linux_daemon_send(&request, response.get(), error.message, sizeof(error.message))) return false;
        if (response->status != SERVICE_STATUS_OK || !response->capture.valid)
            return profile_codec::fail(&error, 0, response->message[0] ? response->message : "capture is unavailable");
        output = response->capture; return true;
    }
    void message(const std::string& text) { fputs(text.c_str(), stdout); fflush(stdout); }
    bool confirm() {
        message("Type IMPORT to commit, or anything else to cancel: ");
        char answer[16]{};
        if (!fgets(answer, sizeof(answer), stdin)) return false;
        return !strcmp(answer, "IMPORT\n") || !strcmp(answer, "IMPORT\r\n");
    }
};

static bool profile_commands_linux(int argc, char** argv, int& exitCode) {
    bool transfer = false, exporting = false;
    for (int i = 1; i < argc; ++i) {
        exporting |= !strcmp(argv[i], "--export-profiles");
        transfer |= exporting || !strcmp(argv[i], "--import-profiles") || !strcmp(argv[i], "--capture-current") || !strcmp(argv[i], "--check-profile");
    }
    if (!transfer) return false;
    char defaultConfig[LINUX_PATH_MAX]{};
    if (!default_linux_config_path(defaultConfig, sizeof(defaultConfig))) snprintf(defaultConfig, sizeof(defaultConfig), "config.ini");
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LinuxProfileCommands io;
    if (profile_check_commands_run(args, io, exitCode)) return true;
    if (profile_capture_commands_run(args, defaultConfig, io, exitCode)) return true;
    if (exporting) {
        ProfileCommand command; ProfileCodecError error; bool handled = false;
        if (!profile_command_parse(args, defaultConfig, command, handled, &error)) {
            io.message(std::string(error.message) + "\n"); exitCode = PROFILE_COMMAND_INVALID; return true;
        }
        LinuxConfigLock lock(command.config.c_str(), error.message, sizeof(error.message));
        if (!lock.locked()) { io.message(std::string(error.message) + "\n"); exitCode = PROFILE_COMMAND_IO; return true; }
        return profile_commands_run(args, defaultConfig, io, exitCode);
    }
    return profile_commands_run(args, defaultConfig, io, exitCode);
}
#endif
