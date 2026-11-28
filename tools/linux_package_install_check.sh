#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
#
# Real install -> upgrade -> remove check of the Linux distro packages with the
# real package managers (rpm, dpkg, pacman), run inside a throwaway overlay of
# the host root so maintainer scripts execute for real (chrooted) and nothing
# persists on the host.
#
# Usage (as root on a Linux host or WSL distro with rpm, dpkg and pacman):
#   tools/linux_package_install_check.sh <old-dir> <new-dir>
# Each directory holds one x86_64 package per format, as written by
# release_manifest.build_linux_distro_packages(); <new-dir> must carry a newer
# version so the upgrade path runs.  Exit status 0 only if every check passes.
#
# Safety: the bind mounts into the overlay are made rslave, so tearing them down
# can never propagate an unmount back to the host's /proc, /sys or /dev.
set -u

if [ "$(id -u)" != 0 ] || [ $# != 2 ]; then
    echo "usage (as root): $0 <old-dir> <new-dir>" >&2
    exit 2
fi
for tool in rpm dpkg pacman chroot useradd; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 2; }
done
OLD=$(cd "$1" && pwd) NEW=$(cd "$2" && pwd)
B=/run/gc-pkgcheck
FAIL=0
TEST_USER=gcpkgcheck
export GREENCURVE_USER=$TEST_USER

pkg() {  # pkg <dir> <glob>: the single matching package, or abort
    set -- "$1"/$2
    [ $# = 1 ] && [ -f "$1" ] || { echo "expected exactly one package for $2" >&2; exit 2; }
    printf '%s' "${1##*/}"
}
pkgver() {  # greencurve-<ver>-<rel>-... -> <ver>
    local base=${1#greencurve-}
    printf '%s' "${base%%-*}"
}
step() { echo; echo "##### $*"; }
check() { if eval "$1"; then echo "  PASS: $2"; else echo "  FAIL: $2"; FAIL=1; fi; }
show() { sed 's/^/  | /'; }

teardown() {
    mountpoint -q $B || return 0
    for d in proc sys dev; do umount -R "$B/m/$d" 2>/dev/null; done
    umount "$B/m" 2>/dev/null
    umount "$B" 2>/dev/null && rmdir "$B"
    if mountpoint -q $B; then echo "teardown left mounts under $B" >&2; FAIL=1; fi
}
setup() {
    teardown
    mkdir -p $B && mount -t tmpfs tmpfs $B && mount --make-private $B
    mkdir -p $B/up $B/work $B/m
    mount -t overlay overlay -o lowerdir=/,upperdir=$B/up,workdir=$B/work $B/m ||
        { echo "overlay mount failed" >&2; exit 2; }
    for d in proc sys dev; do
        mount --rbind "/$d" "$B/m/$d" && mount --make-rslave "$B/m/$d"
    done
    mkdir -p $B/m/pkgs && cp "$OLD"/* "$NEW"/* $B/m/pkgs/
    R=$B/m
    chroot $R useradd -m -u 4242 $TEST_USER 2>/dev/null
    chroot $R groupdel greencurve 2>/dev/null
    true
}
trap teardown EXIT

files_present() {
    test -x $R/usr/bin/greencurve &&
    test -f $R/usr/lib/systemd/system/greencurve.service &&
    test -f $R/usr/lib/systemd/system/greencurve-resume.service &&
    test -f $R/usr/lib/sysusers.d/greencurve.conf &&
    test -f $R/usr/share/applications/greencurve.desktop &&
    test -f $R/usr/share/icons/hicolor/256x256/apps/greencurve.png
}
files_absent() {
    ! test -e $R/usr/bin/greencurve &&
    ! test -e $R/usr/lib/systemd/system/greencurve.service &&
    ! test -e $R/usr/share/doc/greencurve
}
group_ok() { chroot $R getent group greencurve >/dev/null; }
user_enrolled() { chroot $R id -nG $TEST_USER | tr ' ' '\n' | grep -qx greencurve; }

for distro in fedora suse; do
    step "rpm ($distro)"
    setup
    old=$(pkg "$OLD" "greencurve-*-$distro-x86_64.rpm"); new=$(pkg "$NEW" "greencurve-*-$distro-x86_64.rpm")
    rpm --root $R --initdb
    echo "  host rpm _pkgverify_level: $(rpm --eval '%{_pkgverify_level}')"
    # The packages are unsigned.  dnf/zypper skip the signature for a local
    # file (with the documented flag); rpm itself still enforces the digests.
    rpm --root $R -ivh --nodeps --nosignature "$R/pkgs/$old" 2>&1 | show
    check "rpm --root $R -q greencurve >/dev/null" "$distro install registered"
    check files_present "$distro files installed"
    check group_ok "$distro greencurve group created"
    check user_enrolled "$distro user enrolled in greencurve"
    check "rpm --root $R -V --nodeps greencurve" "$distro rpm -V clean"
    rpm --root $R -Uvh --nodeps --nosignature "$R/pkgs/$new" 2>&1 | show
    check "test \$(rpm --root $R -q greencurve | wc -l) = 1" "$distro upgrade leaves one instance"
    check "test \"\$(rpm --root $R -q --qf '%{VERSION}' greencurve)\" = $(pkgver "$new")" "$distro upgraded to $(pkgver "$new")"
    check files_present "$distro files present after upgrade"
    rpm --root $R -e greencurve 2>&1 | show
    check "! rpm --root $R -q greencurve >/dev/null" "$distro removed"
    check files_absent "$distro files gone after removal"
    teardown
done

step "dpkg (Debian/Ubuntu)"
setup
old=$(pkg "$OLD" "greencurve-*-debian-amd64.deb"); new=$(pkg "$NEW" "greencurve-*-debian-amd64.deb")
mkdir -p $R/var/lib/dpkg/info $R/var/lib/dpkg/updates $R/var/lib/dpkg/triggers
: > $R/var/lib/dpkg/status; : > $R/var/lib/dpkg/available
# --force-depends: the overlay's dpkg database has no libc6 entry.
DP="dpkg --root=$R --force-depends --log=/dev/null"
$DP -i "$R/pkgs/$old" 2>&1 | grep -v 'not empty so not removed' | show
check "dpkg --root=$R -s greencurve 2>/dev/null | grep -q 'install ok installed'" "debian install registered"
check files_present "debian files installed"
check group_ok "debian greencurve group created"
check user_enrolled "debian user enrolled in greencurve"
check "dpkg --root=$R -V greencurve" "debian dpkg -V clean (md5sums)"
$DP -i "$R/pkgs/$new" 2>&1 | show
check "dpkg --root=$R -s greencurve | grep -q '^Version: $(pkgver "$new")-'" "debian upgraded to $(pkgver "$new")"
$DP --purge greencurve 2>&1 | grep -v 'not empty so not removed' | show
check "! dpkg --root=$R -s greencurve >/dev/null 2>&1" "debian purged"
check files_absent "debian files gone after purge"
check "test -d $R/usr/bin" "debian left shared directories alone"
teardown

step "pacman (Arch)"
setup
old=$(pkg "$OLD" "greencurve-*-arch-x86_64.pkg.tar.*"); new=$(pkg "$NEW" "greencurve-*-arch-x86_64.pkg.tar.*")
pacman --root $R --noconfirm -U "$R/pkgs/$old" 2>&1 | grep -E '==>|error|warning' | show
check "pacman --root $R -Q greencurve >/dev/null" "arch install registered"
check files_present "arch files installed"
check group_ok "arch greencurve group created"
check user_enrolled "arch user enrolled in greencurve"
pacman --root $R --noconfirm -U "$R/pkgs/$new" 2>&1 | grep -E '==>|error|warning' | show
check "pacman --root $R -Q greencurve | grep -q ' $(pkgver "$new")-'" "arch upgraded to $(pkgver "$new")"
pacman --root $R --noconfirm -R greencurve 2>&1 | grep -E 'error|warning' | show
check files_absent "arch files gone after removal"
teardown

echo
if [ $FAIL = 0 ]; then echo "ALL PACKAGE INSTALL CHECKS PASSED"; else echo "SOME PACKAGE INSTALL CHECKS FAILED"; fi
exit $FAIL
