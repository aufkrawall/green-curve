#!/bin/sh
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
#
# Runs INSIDE one native distro container (started by
# tools/linux_package_container_check.py): installs the old package with the
# distro's own front end, upgrades to the new one, removes it, and checks each
# step.  POSIX sh: not every base image ships bash.
#
#   linux_package_container_inner.sh <deb|fedora|suse|arch> <signed:0|1>
# Mounts: /old and /new (one package set each), /key.asc when signed.
set -u
FMT=$1
SIGNED=$2
FAIL=0
TEST_USER=gcpkgcheck
export GREENCURVE_USER=$TEST_USER

say() { echo "  $*"; }
check() { if eval "$1"; then say "PASS: $2"; else say "FAIL: $2"; FAIL=1; fi; }
one() {  # one <glob>: the single matching file
    set -- $1
    if [ $# != 1 ] || [ ! -f "$1" ]; then echo "expected exactly one package for $*" >&2; exit 2; fi
    printf '%s' "$1"
}
pkgver() { b=${1##*/}; b=${b#greencurve-}; printf '%s' "${b%%-*}"; }
indent() {  # drop dpkg's carriage-return progress meter, keep real output
    tr '\r' '\n' | grep -v -e '^(Reading database \.\.\. *[0-9]*%\{0,1\}$' -e '^$' | sed 's/^/  | /'
}

files_present() {
    test -x /usr/bin/greencurve &&
    test -f /usr/lib/systemd/system/greencurve.service &&
    test -f /usr/lib/systemd/system/greencurve-resume.service &&
    test -f /usr/lib/sysusers.d/greencurve.conf &&
    test -f /usr/share/applications/greencurve.desktop &&
    test -f /usr/share/icons/hicolor/256x256/apps/greencurve.png
}
files_absent() {
    ! test -e /usr/bin/greencurve && ! test -e /usr/lib/systemd/system/greencurve.service &&
    ! test -e /usr/share/doc/greencurve
}
group_ok() { getent group greencurve >/dev/null; }
user_enrolled() { id -nG $TEST_USER | tr ' ' '\n' | grep -qx greencurve; }

case $FMT in
deb)
    OLD=$(one "/old/greencurve-*-debian-*.deb"); NEW=$(one "/new/greencurve-*-debian-*.deb")
    export DEBIAN_FRONTEND=noninteractive
    pm_install() { apt-get install -y --no-install-recommends "$1"; }
    pm_remove() { apt-get purge -y greencurve; }
    installed_ver() { dpkg-query -W -f='${Status} ${Version}' greencurve 2>/dev/null |
                      sed -n 's/^install ok installed \([^-]*\)-.*/\1/p'; }
    pm_verify() { dpkg -V greencurve; }
    instances() { dpkg-query -W greencurve 2>/dev/null | wc -l; }
    ;;
fedora|suse)
    OLD=$(one "/old/greencurve-*-$FMT-*.rpm"); NEW=$(one "/new/greencurve-*-$FMT-*.rpm")
    if [ "$SIGNED" = 1 ]; then
        rpm --import /key.asc
        check "rpm -K '$OLD' | grep -q 'signatures OK'" "rpm -K: signature verifies with the published key"
    fi
    if [ "$FMT" = fedora ]; then
        # Repos off: the only dependency (glibc) is installed, and the run stays
        # hermetic.  Signed packages: force dnf to check local-file signatures.
        DNF="dnf -y --disablerepo=*"
        [ "$SIGNED" = 1 ] && DNF="$DNF --setopt=localpkg_gpgcheck=1"
        pm_install() { $DNF install "$1"; }
        pm_remove() { $DNF remove greencurve; }
    else
        # Repos off: same hermetic contract as dnf --disablerepo=* above;
        # --no-refresh alone still fetches uncached repo metadata in a fresh container.
        # Unsigned packages need the documented flag; signed ones must not.
        zypper --non-interactive modifyrepo --disable --all >/dev/null
        ZYP="zypper --non-interactive --no-refresh"
        ZFLAG=""
        [ "$SIGNED" = 1 ] || ZFLAG="--allow-unsigned-rpm"
        pm_install() { $ZYP install $ZFLAG "$1"; }
        pm_remove() { $ZYP remove greencurve; }
    fi
    # rpm -q prints "package ... is not installed" on stdout, so test first.
    installed_ver() { rpm -q greencurve >/dev/null 2>&1 && rpm -q --qf '%{VERSION}\n' greencurve; }
    pm_verify() { rpm -V greencurve; }
    instances() { rpm -q greencurve 2>/dev/null | grep -c '^greencurve-'; }
    ;;
arch)
    OLD=$(one "/old/greencurve-*-arch-*.pkg.tar.*"); NEW=$(one "/new/greencurve-*-arch-*.pkg.tar.*")
    # The container image strips docs via NoExtract; a real Arch install does
    # not, and pacman -Qkk would report the skipped README as missing.
    sed -i '/^NoExtract/d' /etc/pacman.conf
    pm_install() { pacman -U --noconfirm "$1"; }
    pm_remove() { pacman -R --noconfirm greencurve; }
    installed_ver() { pacman -Q greencurve 2>/dev/null | sed -n 's/^greencurve \([^-]*\)-.*/\1/p'; }
    pm_verify() { pacman -Qkk greencurve >/dev/null; }
    instances() { pacman -Q greencurve 2>/dev/null | wc -l; }
    ;;
*)
    echo "unknown format $FMT" >&2; exit 2 ;;
esac

if ! command -v useradd >/dev/null; then
    echo "image lacks useradd; the check needs a login user to enroll" >&2; exit 2
fi
useradd -m $TEST_USER

say "installing $(basename "$OLD")"
pm_install "$OLD" 2>&1 | indent
check "test \"\$(installed_ver)\" = $(pkgver "$OLD")" "installed $(pkgver "$OLD")"
check files_present "files installed"
check group_ok "greencurve group created"
check user_enrolled "user enrolled in greencurve"
check pm_verify "package manager verification clean"

say "upgrading to $(basename "$NEW")"
pm_install "$NEW" 2>&1 | indent
check "test \"\$(installed_ver)\" = $(pkgver "$NEW")" "upgraded to $(pkgver "$NEW")"
check "test \"\$(instances)\" = 1" "exactly one installed instance"
check files_present "files present after upgrade"
check group_ok "group kept after upgrade"

say "removing"
pm_remove 2>&1 | indent
check "test -z \"\$(installed_ver)\"" "package removed"
check files_absent "files gone after removal"

if [ $FAIL = 0 ]; then say "RESULT: PASS"; else say "RESULT: FAIL"; fi
exit $FAIL
