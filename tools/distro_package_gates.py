# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Source gates for the Linux distro package release path.

Pins the guarantees that make a release's Arch/Debian/Fedora/openSUSE packages
trustworthy, so a workflow edit cannot quietly drop one:

- CI installs every x64 package on native distro containers, and the arm64
  packages on a native arm64 runner;
- the release signs the rpms with a key that must match the published public
  key, refuses to build unsigned (fail closed), and installs the signed x64
  packages on native distros with no signature overrides before publishing;
- the published public key file, when present, holds public material only.
"""
import os

import rpm_sign

PUBLIC_BEGIN = "-----BEGIN PGP PUBLIC KEY BLOCK-----"


def check_distro_package_release_gates(ctx, require_text):
    ci = os.path.join(ctx.SCRIPT_DIR, ".github", "workflows", "ci.yml")
    release = os.path.join(ctx.SCRIPT_DIR, ".github", "workflows", "release.yml")
    inner = os.path.join(ctx.SCRIPT_DIR, "tools", "linux_package_container_inner.sh")
    check = os.path.join(ctx.SCRIPT_DIR, "tools", "linux_package_container_check.py")

    require_text(ci, "python tools/linux_package_container_check.py --engine docker",
                 "CI installs the x64 distro packages on native distro containers")
    require_text(ci, "runs-on: ubuntu-24.04-arm",
                 "CI installs the arm64 distro packages on a native arm64 runner")
    require_text(ci, "--engine docker --arch arm64",
                 "CI arm64 job runs the native container install check")

    require_text(release, "secrets.RPM_SIGNING_KEY",
                 "release signs the rpms with the configured signing key")
    require_text(release, "refusing to publish unsigned rpms",
                 "release fails closed when the rpm signing key is missing")
    require_text(release, "GREENCURVE_REQUIRE_RPM_SIGNATURE=1",
                 "release build refuses to produce unsigned rpms")
    require_text(release, "--show-keys packaging/greencurve-packages.asc",
                 "release checks the signing key against the published public key")
    require_text(release, "trap cleanup_signing_key EXIT",
                 "release removes the signing key when the step ends")
    require_text(release, "linux_package_container_check.py --engine docker --signed",
                 "release installs the signed packages on native distros before publishing")
    require_text(release, "packaging/greencurve-packages.asc \\",
                 "release publishes the rpm public key as an asset")

    # The signed run must refuse every signature override.
    require_text(inner, "--setopt=localpkg_gpgcheck=1",
                 "signed Fedora installs force dnf's local-file signature check")
    require_text(inner, '[ "$SIGNED" = 1 ] || ZFLAG="--allow-unsigned-rpm"',
                 "signed openSUSE installs run without --allow-unsigned-rpm")
    require_text(inner, "signatures OK",
                 "signed runs require rpm -K to report a good signature")
    require_text(check, "@sha256:",
                 "container images are pinned by digest")

    public_key = os.path.join(ctx.SCRIPT_DIR, rpm_sign.PUBLIC_KEY_RELPATH)
    if os.path.exists(public_key):
        with open(public_key, encoding="ascii", errors="replace") as handle:
            text = handle.read()
        if not text.lstrip().startswith(PUBLIC_BEGIN) or "PRIVATE KEY" in text:
            raise RuntimeError(f"{rpm_sign.PUBLIC_KEY_RELPATH} must contain only an armored "
                               "OpenPGP public key")
    else:
        print(f"NOTICE: {rpm_sign.PUBLIC_KEY_RELPATH} is not committed yet; the release "
              "workflow refuses to publish until the rpm signing key is provisioned")
