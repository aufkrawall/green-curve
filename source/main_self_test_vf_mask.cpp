// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// --self-test VF point-mask evidence, split out of main_self_test.cpp (at its
// size ratchet).  Read-only: at most two extra VF status reads, no writes.
//
// The readers send VF status only the points the 128-point table can hold
// (vf_point_mask_policy.h).  This section reports what VF-info advertises and,
// when that reaches past the table, whether status also answers the driver's
// full mask and returns the same table entries.  That is the evidence for a
// board whose driver refuses the full mask outright (reported on an NVIDIA CMP
// 40HX with a modified driver), gathered without any GPU change.

static int self_test_vf_status_read(NvApiFunc getStatus, const VfBackendSpec* backend,
                                    const unsigned char* requestMask, unsigned int maskBytes,
                                    unsigned char* buf) {
    memset(buf, 0, backend->statusBufferSize);
    const unsigned int version = (backend->statusVersion << 16) | backend->statusBufferSize;
    memcpy(buf, &version, sizeof(version));
    memcpy(buf + backend->statusMaskOffset, requestMask, maskBytes);
    return getStatus(g_app.gpuHandle, buf);
}

static void self_test_vf_mask_report(FILE* out) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->readSupported) return;
    unsigned char mask[32] = {};
    const unsigned int maskBytes = (unsigned int)sizeof(mask);
    if (!nvapi_get_vf_info_cached(mask)) {
        fprintf(out, "VF info mask           : not read\n");
        return;
    }
    unsigned char request[sizeof(mask)] = {};
    const unsigned int leftOut = vf_status_request_mask(mask, request, maskBytes, VF_NUM_POINTS);
    fprintf(out, "VF info mask           : %u points advertised (highest %d); %u past point %d"
                 " left out of status requests\n",
            vf_mask_point_count(mask, maskBytes), vf_mask_highest_point(mask, maskBytes),
            leftOut, VF_NUM_POINTS - 1);
    if (leftOut == 0) return;

    auto getStatus = (NvApiFunc)nvapi_qi(backend->getStatusId);
    if (!getStatus || backend->statusBufferSize == 0 || backend->statusBufferSize > 0x4000 ||
        backend->statusMaskOffset + maskBytes > backend->statusBufferSize) {
        fprintf(out, "VF status mask check   : not run (status layout unavailable)\n");
        return;
    }
    HeapBuffer bounded(backend->statusBufferSize);
    HeapBuffer full(backend->statusBufferSize);
    if (!bounded || !full) return;
    const int boundedRet = self_test_vf_status_read(getStatus, backend, request, maskBytes, bounded);
    const int fullRet = self_test_vf_status_read(getStatus, backend, mask, maskBytes, full);
    const bool echoed = boundedRet == 0 &&
        memcmp((unsigned char*)bounded + backend->statusMaskOffset, request, maskBytes) == 0;
    fprintf(out, "VF status, bounded mask: %s (ret=%d, request mask echoed=%s)\n",
            boundedRet == 0 ? "ok" : "REFUSED", boundedRet, self_test_yes_no(echoed));
    if (fullRet != 0) {
        fprintf(out, "VF status, driver mask : REFUSED (ret=%d) - the bounded request is"
                     " what makes the curve readable on this driver\n", fullRet);
        debug_log("self-test: VF status refused the driver mask ret=%d, bounded ret=%d"
                  " (leftOut=%u)\n", fullRet, boundedRet, leftOut);
        return;
    }
    // Entry-by-entry over the table both requests cover.  A temperature step
    // between the two reads can legitimately move a point, so this is evidence,
    // not a verdict.
    unsigned int differing = 0;
    int firstDiffering = -1;
    for (int i = 0; boundedRet == 0 && i < VF_NUM_POINTS; ++i) {
        const unsigned int entry = backend->statusEntriesOffset +
            (unsigned int)i * backend->statusEntryStride;
        if (entry + 8u > backend->statusBufferSize) break;
        if (memcmp((unsigned char*)bounded + entry, (unsigned char*)full + entry, 8) != 0) {
            if (firstDiffering < 0) firstDiffering = i;
            ++differing;
        }
    }
    fprintf(out, "VF status, driver mask : ok (ret=0); points 0..%d %s",
            VF_NUM_POINTS - 1, boundedRet == 0 ? (differing ? "differ" : "identical")
                                               : "not compared");
    if (differing) fprintf(out, " at %u point(s), first %d", differing, firstDiffering);
    fprintf(out, "\n");
    debug_log("self-test: VF status mask check bounded=%d driver=%d echoed=%d differing=%u"
              " first=%d leftOut=%u\n", boundedRet, fullRet, echoed ? 1 : 0, differing,
              firstDiffering, leftOut);
}
