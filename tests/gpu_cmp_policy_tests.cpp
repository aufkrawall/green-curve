// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// NVIDIA CMP mining-board recognition behind the GUI startup warning.

#include <stdio.h>

#include "gpu_cmp_policy.h"

int run_gpu_cmp_policy_tests() {
    // The reported CMP 40HX: packed id and driver name, either alone suffices.
    if (!gpu_is_cmp_mining_board(true, 0x1F0B10DEu, "NVIDIA CMP 40HX")) return 12220;
    if (!gpu_is_cmp_mining_board(true, 0x1F0B10DEu, "NVIDIA GeForce RTX 2060")) return 12221;
    if (!gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP 40HX")) return 12222;
    // Other CMP parts by name, without guessed device ids.
    if (!gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP 170HX") ||
        !gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP 30HX") ||
        !gpu_is_cmp_mining_board(false, 0, "nvidia cmp 90hx")) return 12223;
    // The id needs valid PCI data and the NVIDIA vendor word.
    if (gpu_is_cmp_mining_board(false, 0x1F0B10DEu, "NVIDIA GeForce RTX 2060") ||
        gpu_is_cmp_mining_board(true, 0x1F0B1002u, "Some other adapter")) return 12224;
    // Ordinary boards, including the same TU106 die as a GeForce, never match.
    if (gpu_is_cmp_mining_board(true, 0x1F0810DEu, "NVIDIA GeForce RTX 2060") ||
        gpu_is_cmp_mining_board(true, 0x2F0410DEu, "NVIDIA GeForce RTX 5070") ||
        gpu_is_cmp_mining_board(true, 0, nullptr) ||
        gpu_is_cmp_mining_board(false, 0, "")) return 12225;
    // Only the whole "CMP <n>HX" word matches.
    if (gpu_is_cmp_mining_board(false, 0, "NVIDIA XCMP 40HX") ||
        gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP HX") ||
        gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP 1234HX") ||
        gpu_is_cmp_mining_board(false, 0, "NVIDIA CMP 40HXL") ||
        gpu_is_cmp_mining_board(false, 0, "NVIDIA COMPUTE 40HX") ||
        gpu_is_cmp_mining_board(false, 0, "CMP")) return 12226;
    if (!gpu_is_cmp_mining_board(false, 0, "CMP 50HX (modified driver)")) return 12227;
    return 0;
}

#ifdef GC_CMP_POLICY_TEST_MAIN
int main() {
    const int result = run_gpu_cmp_policy_tests();
    printf("GPU CMP policy regression: %d\n", result);
    return result;
}
#endif
