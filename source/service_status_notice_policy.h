// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// How the background-service status line carries its extra notices.
//
// Up to four notices used to be appended IN FULL to the one-line service status
// label: the shared-profiles restriction, the install-folder protection warning
// (unprotected, network, or no file permissions) and the user-profile-folder
// warning.  Each is a sentence or two, so on an unsafe install the label ran to
// several hundred characters in a control one line tall and read as broken,
// clipped text.  The label now names each notice in a few words and says there
// is more; the full sentences live in the label's hover tooltip.
//
// ASCII only: the status label is set through the ANSI window API.

#ifndef GREEN_CURVE_SERVICE_STATUS_NOTICE_POLICY_H
#define GREEN_CURVE_SERVICE_STATUS_NOTICE_POLICY_H

#include <stddef.h>
#include <string.h>

struct ServiceStatusNotices {
    bool sharedProfilesOnly;
    bool folderWithoutPermissions;
    bool networkFolder;
    bool folderWritableByStandardUsers;
    bool underUserProfile;
};

struct ServiceStatusNoticeText {
    const char* shortText;
    const char* fullText;
    bool warning;
};

static inline size_t service_status_notice_list(const ServiceStatusNotices& n,
    ServiceStatusNoticeText* out, size_t outCount) {
    size_t count = 0;
    auto add = [&](bool on, const char* shortText, const char* fullText, bool warning) {
        if (on && count < outCount) out[count++] = { shortText, fullText, warning };
    };
    add(n.sharedProfilesOnly, "shared profiles only",
        "An administrator restricts this PC to shared profiles; use "
        "'Shared profiles...' to apply one.", false);
    add(n.folderWithoutPermissions, "install folder cannot be protected",
        "This drive has no file permissions, so the Green Curve files cannot be "
        "protected.", true);
    add(n.networkFolder, "network install folder",
        "Green Curve runs from a network folder. The server controls its files "
        "and could replace the background service.", true);
    add(n.folderWritableByStandardUsers, "unprotected install folder",
        "This install folder can be changed by non-administrators, who could "
        "then replace the LocalSystem background service and gain SYSTEM "
        "rights.", true);
    add(n.underUserProfile, "user-folder install",
        "Green Curve is running from a user account folder, so restricted or "
        "standard users on this PC cannot launch it. Reinstall under an "
        "all-users folder such as %ProgramFiles%\\greencurve to make it "
        "available to all users.", true);
    return count;
}

static inline void service_status_append(char* out, size_t outSize, const char* text) {
    if (!out || outSize == 0 || !text) return;
    size_t used = strlen(out);
    size_t add = strlen(text);
    if (used + add >= outSize) add = outSize - 1 - used;
    memcpy(out + used, text, add);
    out[used + add] = 0;
}

// `label` = base + " Warning: a, b (hover for details)." when any notice is
// present; `tooltip` = the full sentences, one paragraph each, or "".
static inline void service_status_compose(const char* base,
    const ServiceStatusNotices& notices, char* label, size_t labelSize,
    char* tooltip, size_t tooltipSize) {
    if (label && labelSize) label[0] = 0;
    if (tooltip && tooltipSize) tooltip[0] = 0;
    service_status_append(label, labelSize, base ? base : "");
    ServiceStatusNoticeText list[5] = {};
    size_t count = service_status_notice_list(notices, list, 5);
    if (count == 0) return;
    bool anyWarning = false;
    for (size_t i = 0; i < count; ++i) anyWarning = anyWarning || list[i].warning;
    service_status_append(label, labelSize, anyWarning ? " Warning: " : " Note: ");
    for (size_t i = 0; i < count; ++i) {
        if (i) service_status_append(label, labelSize, ", ");
        service_status_append(label, labelSize, list[i].shortText);
        if (i) service_status_append(tooltip, tooltipSize, "\r\n\r\n");
        service_status_append(tooltip, tooltipSize, list[i].fullText);
    }
    service_status_append(label, labelSize, " (hover for details).");
}

#endif  // GREEN_CURVE_SERVICE_STATUS_NOTICE_POLICY_H
