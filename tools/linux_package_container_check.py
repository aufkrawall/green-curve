# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Install -> upgrade -> remove the Linux distro packages on native distros.

Each supported distribution runs in its own container (docker or podman) from
a digest-pinned image, using that distro's own front end: apt on Debian and
Ubuntu, dnf on Fedora, zypper on openSUSE, pacman on Arch.  Dependency
resolution, maintainer scripts, group creation, user enrollment, signature
checks (when the rpms are signed), upgrade and removal are all exercised for
real.  CI runs this on every push; the release workflow runs it before
anything is published.

    python tools/linux_package_container_check.py --packages dist/linux-x64 \\
        --binary dist/linux-x64/greencurve/greencurve [--signed] [--only fedora-45]

The upgrade set is built here from the same binary with a higher version, and
signed with the same key when --signed (GREENCURVE_RPM_SIGNING_KEY).  With
--new-packages DIR a pre-built higher-version set is used instead, so a signed
check can run where the private key is not available.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import release_manifest  # noqa: E402
import rpm_sign  # noqa: E402

SCRIPT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INNER = os.path.join(SCRIPT_DIR, "tools", "linux_package_container_inner.sh")

# (name, package format, image pinned to a multi-arch index digest).  Covers
# the oldest and newest supported releases of each family.  Bump digests
# deliberately; a moving tag would make the release gate non-reproducible.
DISTROS = (
    ("debian-12", "deb", "docker.io/library/debian@sha256:1d04200c2ece1d4d75a2a6f8e11ebce7d86a2ae28b34bb2c9cbb3ee9f4953e75"),
    ("debian-13", "deb", "docker.io/library/debian@sha256:3cda3ca472ce45507a5109c6a3907093e24d9ae67d55954c9a07a45665dafba2"),
    ("ubuntu-22.04", "deb", "docker.io/library/ubuntu@sha256:b000e69a50ff940390233025e7a337fb8469b509e2334fa4d018ac30f3e18cdb"),
    ("ubuntu-24.04", "deb", "docker.io/library/ubuntu@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b"),
    ("ubuntu-26.04", "deb", "docker.io/library/ubuntu@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db"),
    ("fedora-44", "fedora", "docker.io/library/fedora@sha256:15da97ed9aa94dd3d92c3925eb89520f6c80e07ea7ee24cba848d1f8b6912b3e"),
    ("fedora-45", "fedora", "docker.io/library/fedora@sha256:a7d72ba2745b100a3351087472e5ee7b2450af7cf331e7673007e3817612839c"),
    ("opensuse-leap-15.6", "suse", "docker.io/opensuse/leap@sha256:79be7751205ea84559990fb76b1bec71e38d6fad41c70a4f6c921b803b58f421"),
    ("opensuse-leap-16.0", "suse", "docker.io/opensuse/leap@sha256:a4115e2f5aea9712a7c73974fd94000a034fc3051ef29b5f7722fbf04070dd9d"),
    ("opensuse-tumbleweed", "suse", "docker.io/opensuse/tumbleweed@sha256:e2e21eb6f551a3e5682538530facc198aa9fa753dc84a4f9845b3134777afd8d"),
    ("archlinux", "arch", "docker.io/library/archlinux@sha256:375a401bb3c6376b37f40b6dfa25067db37ec78d0b7af67ae6315f43a0184a9d"),
)
# archlinux images exist for x86_64 only.
X64_ONLY = frozenset(("archlinux",))
CONTAINER_TIMEOUT_S = 900


def find_engine(preferred):
    for name in ([preferred] if preferred else ["docker", "podman"]):
        path = shutil.which(name)
        if path:
            return path
    raise RuntimeError("no container engine found (docker or podman)")


def read_version():
    with open(os.path.join(SCRIPT_DIR, "VERSION"), encoding="utf-8") as handle:
        return handle.read().strip()


def stage_old_set(packages_dir, version, arch, dest):
    """Copy exactly the four distro packages of `version` into `dest`."""
    os.makedirs(dest, exist_ok=True)
    names = sorted(n for n in os.listdir(packages_dir)
                   if n.startswith(f"greencurve-{version}-1-") and not n.endswith(".sha256"))
    if len(names) != 4:
        raise RuntimeError(f"expected 4 distro packages for {version} in {packages_dir}, found {names}")
    for name in names:
        shutil.copy2(os.path.join(packages_dir, name), dest)
    return names


def run_distro(engine, name, fmt, image, old_dir, new_dir, signed, platform):
    cmd = [engine, "run", "--rm", "--pull=missing",
           "-v", f"{old_dir}:/old:ro", "-v", f"{new_dir}:/new:ro",
           "-v", f"{INNER}:/check.sh:ro"]
    if platform:
        cmd += ["--platform", platform]
    if signed:
        cmd += ["-v", f"{os.path.join(SCRIPT_DIR, rpm_sign.PUBLIC_KEY_RELPATH)}:/key.asc:ro"]
    cmd += [image, "sh", "/check.sh", fmt, "1" if signed else "0"]
    start = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                                timeout=CONTAINER_TIMEOUT_S)
        output, code = result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired as exc:
        output, code = (exc.stdout or "") + f"\n  TIMEOUT after {CONTAINER_TIMEOUT_S}s", 124
    return code, output, time.monotonic() - start


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--packages", required=True, help="dist/linux-<arch> with the release packages")
    parser.add_argument("--binary", required=True, help="the Linux binary used for the upgrade set")
    parser.add_argument("--arch", default="x64", choices=("x64", "arm64"))
    parser.add_argument("--engine", help="docker or podman (default: first found)")
    parser.add_argument("--signed", action="store_true",
                        help="the rpms are signed: import the published key, and fail unless "
                             "dnf/zypper/rpm accept them without signature overrides")
    parser.add_argument("--only", help="comma-separated distro names")
    parser.add_argument("--new-packages",
                        help="pre-built higher-version package set to upgrade to "
                             "(default: build <VERSION>.1 here)")
    args = parser.parse_args(argv)

    engine = find_engine(args.engine)
    version = read_version()
    wanted = set(args.only.split(",")) if args.only else None
    distros = [d for d in DISTROS if (wanted is None or d[0] in wanted)
               and not (args.arch == "arm64" and d[0] in X64_ONLY)]
    if wanted and len(distros) != len(wanted):
        raise SystemExit(f"unknown distro in --only: {sorted(wanted - {d[0] for d in distros})}")
    if args.signed and not args.new_packages and not os.environ.get(rpm_sign.SIGNING_KEY_ENV):
        raise SystemExit(f"--signed needs {rpm_sign.SIGNING_KEY_ENV} to sign the upgrade set "
                         "(or a signed --new-packages set)")
    if not args.signed:
        # The upgrade set must match the release set: unsigned here means unsigned there.
        os.environ.pop(rpm_sign.SIGNING_KEY_ENV, None)

    work = tempfile.mkdtemp(prefix="gc-pkgcontainer-")
    try:
        old_dir, new_dir = os.path.join(work, "old"), os.path.join(work, "new")
        names = stage_old_set(args.packages, version, args.arch, old_dir)
        print(f"  pkg-container: engine={os.path.basename(engine)} arch={args.arch} "
              f"signed={int(args.signed)} old={version} ({len(names)} packages)")
        if args.new_packages:
            os.makedirs(new_dir)
            new_names = [n for n in os.listdir(args.new_packages)
                         if n.startswith("greencurve-") and not n.endswith(".sha256")
                         and not n.startswith(f"greencurve-{version}-")]
            if len(new_names) != 4:
                raise SystemExit(f"--new-packages must hold 4 packages of a newer version, "
                                 f"found {sorted(new_names)}")
            for name in new_names:
                shutil.copy2(os.path.join(args.new_packages, name), new_dir)
        else:
            release_manifest.build_linux_distro_packages(
                SCRIPT_DIR, version + ".1", args.arch, args.binary, new_dir)
        for name in os.listdir(new_dir):
            if name.endswith(".sha256"):
                os.remove(os.path.join(new_dir, name))
        platform = {"x64": "linux/amd64", "arm64": "linux/arm64"}[args.arch]
        results = []
        for name, fmt, image in distros:
            print(f"\n##### {name} ({fmt}) {image.split('@')[0]}", flush=True)
            code, output, seconds = run_distro(engine, name, fmt, image, old_dir, new_dir,
                                               args.signed, platform)
            print(output.rstrip())
            results.append((name, code, seconds))
            print(f"  pkg-container: {name}: {'PASS' if code == 0 else f'FAIL (exit {code})'} "
                  f"in {seconds:.0f}s", flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    failed = [name for name, code, _ in results if code != 0]
    print(f"\n  pkg-container: {len(results) - len(failed)}/{len(results)} distros passed"
          + (f"; FAILED: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
