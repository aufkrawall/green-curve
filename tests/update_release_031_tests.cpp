// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include <string.h>

#include "update_manifest_policy.h"

int run_update_release_031_tests() {
    static const char manifestText[] =
        "format=1\n"
        "version=0.31.0\n"
        "x64_file=greencurve-0.31.0-windows-x64-setup.exe\n"
        "x64_size=200000\n"
        "x64_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "arm64_file=greencurve-0.31.0-windows-arm64-setup.exe\n"
        "arm64_size=300000\n"
        "arm64_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";
    GcUpdateManifest manifest = {};
    gc_update_manifest_parse(manifestText, strlen(manifestText), &manifest);
    if (!manifest.valid || manifest.hasMinimumFrom) return 7670;

    // Every public updater-bearing release remains eligible, including the
    // short version spellings retained in the older release asset names.
    const char* const older[] = {"0.23", "0.23.1", "0.24.0", "0.25.0",
        "0.25.1", "0.25.2", "0.26.0", "0.27.0", "0.28", "0.29.0", "0.30",
        "0.30.0"};
    const GcUpdateArch arches[] = {GC_UPDATE_ARCH_X64, GC_UPDATE_ARCH_ARM64};
    const char* const current[] = {"0.31", "0.31.0"};
    const char* const newer[] = {"0.31.1", "0.32"};
    for (GcUpdateArch arch : arches) {
        for (const char* text : older) {
            GcUpdateVersion installed = {};
            gc_update_version_parse(text, &installed);
            if (!installed.valid || gc_update_decide(&manifest, &installed, arch) !=
                    GC_UPDATE_DECISION_AVAILABLE) return 7671;
        }
        for (const char* text : current) {
            GcUpdateVersion installed = {};
            gc_update_version_parse(text, &installed);
            if (gc_update_decide(&manifest, &installed, arch) !=
                    GC_UPDATE_DECISION_UP_TO_DATE) return 7672;
        }
        for (const char* text : newer) {
            GcUpdateVersion installed = {};
            gc_update_version_parse(text, &installed);
            if (gc_update_decide(&manifest, &installed, arch) !=
                    GC_UPDATE_DECISION_REJECTED) return 7673;
        }
        char filename[GC_UPDATE_ASSET_NAME_MAX_CHARS] = {};
        const GcUpdateAsset* asset = gc_update_select_asset(&manifest, arch);
        if (!asset || !gc_update_expected_asset_name(manifest.version.text, arch,
                filename, sizeof(filename)) || strcmp(asset->file, filename)) return 7674;
        const char* expected = arch == GC_UPDATE_ARCH_X64
            ? "greencurve-0.31.0-windows-x64-setup.exe"
            : "greencurve-0.31.0-windows-arm64-setup.exe";
        if (strcmp(filename, expected)) return 7675;
    }
    return 0;
}
