// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once

// Shared by both generated units. Packaged units are checked against this list.
// Driver access requires devices and writable driver state; full keeps /usr
// read-only without restricting the NVIDIA runtime as strict would.
#define GC_LINUX_SERVICE_SANDBOX \
    "UMask=0077\n" \
    "NoNewPrivileges=true\n" \
    "ProtectSystem=full\n" \
    "ProtectHome=yes\n" \
    "PrivateTmp=yes\n" \
    "ProtectControlGroups=yes\n" \
    "ProtectKernelLogs=yes\n" \
    "RestrictSUIDSGID=yes\n" \
    "RestrictNamespaces=yes\n" \
    "RestrictRealtime=yes\n" \
    "RestrictAddressFamilies=AF_UNIX\n" \
    "SystemCallArchitectures=native\n" \
    "LockPersonality=yes\n"
