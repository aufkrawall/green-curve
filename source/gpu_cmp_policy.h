// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// gpu_cmp_policy.h — recognizes an NVIDIA CMP (Crypto Mining Processor) board.
// Pure; used by the GUI startup warning and the self-test report.
//
// CMP boards are Turing/Ampere mining silicon without display outputs, sold
// with a compute-only driver.  Tuning one needs a modified NVIDIA driver, and
// Green Curve's support is community-reported on a CMP 40HX (TU106) only.  Its
// architecture id is ordinary (Turing), so backend selection is unaffected;
// what differs is the driver, which is why the GUI asks the user to accept an
// explicit warning (main_capability_warning.cpp).
//
// Identity, either of:
//  - a PCI device id confirmed from a user report.  NvAPI packs the identifier
//    as (device << 16) | vendor, e.g. 0x1F0B10DE for the CMP 40HX.  Ids that
//    have not been confirmed are deliberately not listed.
//  - the board name the driver reports, "NVIDIA CMP <n>HX".  That covers the
//    other CMP parts without guessing their ids; the id covers a modified
//    driver that renames the board.

#ifndef GREEN_CURVE_GPU_CMP_POLICY_H
#define GREEN_CURVE_GPU_CMP_POLICY_H

static const unsigned int GPU_CMP_PCI_VENDOR_NVIDIA = 0x10DEu;

static const unsigned int g_gpuCmpConfirmedDeviceIds[] = {
    0x1F0Bu,  // CMP 40HX (TU106), confirmed by nvidia-smi on a user's board
};

static inline char gpu_cmp_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// "CMP " + 1..3 digits + "HX" as a whole word, ASCII case-insensitive.
static inline bool gpu_cmp_name_matches(const char* name) {
    if (!name) return false;
    for (const char* p = name; *p; ++p) {
        if (p != name && *(p - 1) != ' ') continue;
        if (gpu_cmp_ascii_upper(p[0]) != 'C' || gpu_cmp_ascii_upper(p[1]) != 'M' ||
            gpu_cmp_ascii_upper(p[2]) != 'P' || p[3] != ' ') continue;
        const char* q = p + 4;
        int digits = 0;
        while (q[digits] >= '0' && q[digits] <= '9') ++digits;
        if (digits < 1 || digits > 3) continue;
        q += digits;
        if (gpu_cmp_ascii_upper(q[0]) != 'H' || gpu_cmp_ascii_upper(q[1]) != 'X') continue;
        if (q[2] == '\0' || q[2] == ' ') return true;
    }
    return false;
}

static inline bool gpu_cmp_device_id_confirmed(bool pciInfoValid, unsigned int nvapiDeviceId) {
    if (!pciInfoValid || (nvapiDeviceId & 0xFFFFu) != GPU_CMP_PCI_VENDOR_NVIDIA) return false;
    const unsigned int device = (nvapiDeviceId >> 16) & 0xFFFFu;
    for (unsigned int id : g_gpuCmpConfirmedDeviceIds) {
        if (id == device) return true;
    }
    return false;
}

static inline bool gpu_is_cmp_mining_board(bool pciInfoValid, unsigned int nvapiDeviceId,
                                           const char* name) {
    return gpu_cmp_device_id_confirmed(pciInfoValid, nvapiDeviceId) ||
           gpu_cmp_name_matches(name);
}

#endif // GREEN_CURVE_GPU_CMP_POLICY_H
