// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Getter-only cases, included inside the serialized daemon command switch.
case SERVICE_CMD_CAPTURE_CURRENT:
    daemon_handle_capture(req, resp);
    break;
case SERVICE_CMD_GET_ACTIVE_DESIRED:
    if (g_hasActiveDesired) resp->desired = g_activeDesired;
    resp->status = SERVICE_STATUS_OK;
    break;
