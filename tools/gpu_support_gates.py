# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Source gates for VF status point masks and the CMP mining-board warning.

Lives here rather than in build.py so the build script stays under its size
ratchet; build.py passes its own require_text/forbid_text in, and this module
never imports build.py.
"""

import os


def check_source_gates(ctx, require_text, forbid_text):
    src = ctx.SOURCE_DIR
    # Every VF STATUS reader sends only the points the 128-point table can hold
    # (vf_point_mask_policy.h).  The full advertised mask makes a driver that
    # cannot serve a requested point refuse the whole read.
    for name in ("gpu_backend.cpp", "linux_backend_vf_read.h", "linux_gpu.cpp",
                 "hardware_capture_getters.h", "main_self_test_vf_mask.cpp"):
        require_text(os.path.join(src, name), "vf_status_request_mask(",
                     f"{name} bounds its VF status request to the point table")
    forbid_text(os.path.join(src, "gpu_backend.cpp"),
                "buf.write_at(backend->statusMaskOffset, mask, sizeof(mask))",
                "the Windows reader never sends the unbounded driver mask to status")
    forbid_text(os.path.join(src, "linux_backend_vf_read.h"),
                "memcpy(buf + b->statusMaskOffset, g->vfMask",
                "the Linux reader never sends the unbounded driver mask to status")
    require_text(os.path.join(src, "main_vf_info_cache.cpp"), "pastTable=",
                 "the VF-info cache logs how far the mask reaches past the table")

    # CMP mining boards: an abortable startup tier with its own opt-out.
    warning = os.path.join(src, "main_capability_warning.cpp")
    require_text(warning, "if (!show_cmp_board_warning(parent)) return false;",
                 "the CMP warning is reached through the single startup call site")
    require_text(warning, "\"hide_cmp_board_warning\"",
                 "the CMP warning has its own persistent opt-out")
    require_text(warning, "TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON",
                 "the CMP warning can be continued once or exited")
    require_text(warning, "pszVerificationText = L\"Do not show this warning again for CMP cards\"",
                 "the CMP warning can be skipped for good")
    require_text(os.path.join(src, "gpu_cmp_policy.h"), "0x1F0Bu",
                 "the confirmed CMP 40HX device id stays recognized")
