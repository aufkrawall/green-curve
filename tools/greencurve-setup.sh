#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
#
# Green Curve Linux setup: install, verify, inspect or remove the GPU control
# daemon.
#
# The binary already owns the privileged work (`--service-install` stages the
# root-owned executable, creates the greencurve group, writes and verifies the
# systemd unit, and proves the socket permissions).  This script is the wrapper
# around it: it runs that step, performs the one thing the binary deliberately
# refuses to do on its own -- adding an account to the greencurve group -- and
# installs the desktop entries.
#
#   sudo ./greencurve-setup.sh install
#   ./greencurve-setup.sh status
#   sudo ./greencurve-setup.sh uninstall
#
# Uninstall never removes persisted settings unless --purge is passed: a
# reinstall is the common case and silently discarding a tuned curve is not
# recoverable.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BINARY="${GREENCURVE_BINARY:-$SCRIPT_DIR/greencurve}"
GROUP_NAME="greencurve"
UNIT_NAME="greencurve.service"
UNIT_PATH="/etc/systemd/system/$UNIT_NAME"
INSTALL_DIR="/usr/local/libexec/greencurve"
BIN_DIR="/usr/local/bin"
BIN_PATH="$BIN_DIR/greencurve"
STATE_DIR="/var/lib/greencurve"
SOCKET_PATH="/run/greencurve/greencurve.sock"

PURGE=0

say()  { printf '%s\n' "$*"; }
info() { printf '  %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

require_root() {
    [ "$(id -u)" -eq 0 ] || die "'$1' requires root; re-run with sudo."
}

require_binary() {
    [ -x "$BINARY" ] || die "cannot find an executable greencurve at '$BINARY'.
Run this script from the extracted archive, or set GREENCURVE_BINARY=/path/to/greencurve."
}

# A candidate must be a real, existing, non-root login account.  Enrolling an
# account in this group grants it GPU overclock/undervolt control through the
# daemon socket, so a system account or a wrong guess is not a harmless mistake.
usable_account() {
    local name="${1:-}"
    [ -n "$name" ] || return 1
    [ "$name" != "root" ] || return 1
    local uid
    uid="$(id -u "$name" 2>/dev/null)" || return 1
    # Below 1000 is the system-account range on every distro this ships for.
    [ "$uid" -ge 1000 ] 2>/dev/null || return 1
    return 0
}

# The account to enroll: the user who invoked sudo, not root.  Falls back to the
# owner of the login session, then to the sole active session.
#
# Resolution is deliberately conservative and ordered from most to least
# explicit.  It gives up rather than choosing between candidates: an empty
# result is not a failure, it makes cmd_install print the manual usermod line.
# Guessing wrong would hand GPU control to an account nobody asked for.
target_user() {
    local candidate=""

    # 1. An explicit request always wins.
    if usable_account "${GREENCURVE_USER:-}"; then
        printf '%s' "$GREENCURVE_USER"; return 0
    fi

    # 2. sudo records exactly who asked.
    if usable_account "${SUDO_USER:-}"; then
        printf '%s' "$SUDO_USER"; return 0
    fi

    # 3. A plain root shell (su -, sudo -i, a root console) clears SUDO_USER.
    #    logname reports the owner of the login session, which is the account
    #    that will actually run the client.  This is the case that previously
    #    fell through to "could not determine which account".
    candidate="$(logname 2>/dev/null || true)"
    if usable_account "$candidate"; then
        printf '%s' "$candidate"; return 0
    fi

    # 4. Last resort: exactly one non-root account with an active session.
    #    Two different users logged in is ambiguous, so that yields nothing.
    if command -v loginctl >/dev/null 2>&1; then
        candidate="$(loginctl list-sessions --no-legend 2>/dev/null |
                     awk '{print $3}' | sort -u | grep -vx root || true)"
        if [ "$(printf '%s' "$candidate" | grep -c .)" -eq 1 ] &&
           usable_account "$candidate"; then
            printf '%s' "$candidate"; return 0
        fi
    fi

    printf ''
}

cmd_install() {
    require_root install
    require_binary

    local user
    user="$(target_user)"

    say "Installing the Green Curve daemon..."
    # Everything security-relevant happens in here: staging, group creation,
    # unit generation, restart, and the socket/protocol verification.  A
    # non-zero exit means the daemon is NOT verified, so stop rather than
    # continuing to the convenience steps.
    #
    # The group cannot exist until this step creates it, so enrollment has to
    # come after -- which means the binary's own "run usermod" advice would
    # print immediately before this script ran usermod.  Claim ownership of that
    # message whenever we actually have an account to enroll.
    if [ -n "$user" ]; then
        GREENCURVE_SETUP_OWNS_GROUP=1 "$BINARY" --service-install ||
            die "daemon installation failed; nothing else was changed."
    else
        "$BINARY" --service-install ||
            die "daemon installation failed; nothing else was changed."
    fi

    if [ -z "$user" ]; then
        warn "could not determine which account to add to the '$GROUP_NAME' group."
        say  "Run: sudo usermod -aG $GROUP_NAME <your-user>"
    elif id -nG "$user" 2>/dev/null | tr ' ' '\n' | grep -qx "$GROUP_NAME"; then
        info "user '$user' is already in the '$GROUP_NAME' group"
    else
        say "Adding '$user' to the '$GROUP_NAME' group..."
        usermod -aG "$GROUP_NAME" "$user" ||
            die "usermod failed; add the group manually: sudo usermod -aG $GROUP_NAME $user"
        info "added; group membership takes effect after a new login"
        info "for this session only: newgrp $GROUP_NAME"
    fi

    say "Installing command link to $BIN_PATH..."
    install -d -m 0755 "$BIN_DIR"
    ln -sf "$INSTALL_DIR/greencurve" "$BIN_PATH"
    info "linked $BIN_PATH -> $INSTALL_DIR/greencurve"

    install_desktop_entries "$user"

    say ""
    say "Done. Next steps:"
    info "greencurve --probe        confirm the driver and GPU are visible"
    info "greencurve --tui          open the terminal UI"
    info "greencurve --show-startup show what is applied when the daemon starts"
}

# Desktop entries are per-user, so they are written into the invoking user's
# home rather than a system-wide location: the daemon is machine-wide, but the
# launcher belongs to whoever ran the install.
install_desktop_entries() {
    local user="$1"
    [ -n "$user" ] || return 0
    local home
    home="$(getent passwd "$user" | cut -d: -f6)"
    [ -n "$home" ] && [ -d "$home" ] || { warn "no home directory for '$user'; skipping desktop entry"; return 0; }

    local apps="$home/.local/share/applications"
    local entry="$apps/greencurve.desktop"
    local target_bin="$INSTALL_DIR/greencurve"
    if [ -x "$BIN_PATH" ]; then
        target_bin="$BIN_PATH"
    elif [ -x "$BINARY" ]; then
        target_bin="$BINARY"
    fi
    local exec_binary="${target_bin//\\/\\\\}"
    exec_binary="${exec_binary//\"/\\\"}"
    command -v runuser >/dev/null 2>&1 ||
        { warn "runuser is unavailable; skipping desktop entry"; return 0; }
    # Everything below runs with the target account's privileges. The desktop
    # tree is user-controlled, so a root shell must never follow a planted
    # directory or file symlink while creating this convenience launcher.
    if ! runuser -u "$user" -- install -d -m 0755 "$apps"; then
        warn "could not create '$apps' as '$user'; skipping desktop entry"
        return 0
    fi
    if ! runuser -u "$user" -- sh -c '
        set -eu
        directory=$1
        temporary=$(mktemp -- "$directory/.greencurve.desktop.XXXXXX")
        trap '"'"'rm -f -- "$temporary"'"'"' EXIT
        cat > "$temporary"
        chmod 0644 "$temporary"
        mv -fT -- "$temporary" "$directory/greencurve.desktop"
        trap - EXIT
    ' sh "$apps" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Green Curve
Comment=NVIDIA VF curve, overclock, undervolt and fan control
Exec="$exec_binary" --tui --from-desktop
Icon=greencurve
Terminal=true
Categories=Utility;System;Settings;
StartupNotify=false
EOF
    then
        warn "could not write '$entry' as '$user'; skipping desktop entry"
        return 0
    fi
    info "desktop entry written to $entry"
}

cmd_uninstall() {
    require_root uninstall
    require_binary

    say "Removing the Green Curve daemon..."
    "$BINARY" --service-remove || warn "--service-remove reported a failure; continuing with cleanup"

    # The binary removes the unit; these are the leftovers it deliberately keeps
    # so that an upgrade does not lose anything.
    rm -f "$UNIT_PATH"
    systemctl daemon-reload 2>/dev/null || true
    if [ -L "$BIN_PATH" ] || [ -f "$BIN_PATH" ]; then
        rm -f "$BIN_PATH"
        info "removed $BIN_PATH"
    fi
    if [ -d "$INSTALL_DIR" ]; then
        rm -rf "$INSTALL_DIR"
        info "removed $INSTALL_DIR"
    fi

    local user
    user="$(target_user)"
    if [ -n "$user" ]; then
        local home
        home="$(getent passwd "$user" 2>/dev/null | cut -d: -f6)"
        if [ -n "$home" ] && [ -f "$home/.local/share/applications/greencurve.desktop" ]; then
            rm -f "$home/.local/share/applications/greencurve.desktop"
            info "removed desktop entry for '$user'"
        fi
    fi

    if [ "$PURGE" -eq 1 ]; then
        # Explicitly requested: this discards the committed intent, the boot
        # policy and the operation journal.
        if [ -d "$STATE_DIR" ]; then
            rm -rf "$STATE_DIR"
            info "purged $STATE_DIR (committed settings, startup policy, operation journal)"
        fi
        if getent group "$GROUP_NAME" >/dev/null; then
            groupdel "$GROUP_NAME" 2>/dev/null &&
                info "removed the '$GROUP_NAME' group" ||
                warn "could not remove the '$GROUP_NAME' group (still has members?)"
        fi
    else
        [ -d "$STATE_DIR" ] && info "kept $STATE_DIR (pass --purge to remove it)"
        getent group "$GROUP_NAME" >/dev/null &&
            info "kept the '$GROUP_NAME' group (pass --purge to remove it)"
    fi
    say "Done."
}

cmd_status() {
    say "Green Curve daemon status"
    if [ -x "$BINARY" ]; then
        info "client binary: $BINARY"
    else
        info "client binary: not found at $BINARY"
    fi
    if [ -e "$BIN_PATH" ]; then
        info "command link: $BIN_PATH -> $(readlink -f "$BIN_PATH" 2>/dev/null || echo "$INSTALL_DIR/greencurve")"
    else
        info "command link: not installed at $BIN_PATH"
    fi
    if [ -f "$UNIT_PATH" ]; then
        info "unit: $UNIT_PATH"
        info "enabled: $(systemctl is-enabled "$UNIT_NAME" 2>/dev/null || echo no)"
        info "active:  $(systemctl is-active "$UNIT_NAME" 2>/dev/null || echo no)"
    else
        info "unit: not installed"
    fi
    if [ -e "$SOCKET_PATH" ]; then
        info "socket: $(stat -c '%n owner=%U group=%G mode=%a type=%F' "$SOCKET_PATH")"
    else
        info "socket: $SOCKET_PATH is absent (daemon not running?)"
    fi
    if getent group "$GROUP_NAME" >/dev/null; then
        info "group '$GROUP_NAME': present (members: $(getent group "$GROUP_NAME" | cut -d: -f4 | sed 's/^$/none/'))"
        if id -nG 2>/dev/null | tr ' ' '\n' | grep -qx "$GROUP_NAME"; then
            info "this shell HAS the '$GROUP_NAME' group"
        else
            info "this shell does NOT have the '$GROUP_NAME' group (log out and back in, or run: newgrp $GROUP_NAME)"
        fi
    else
        info "group '$GROUP_NAME': missing"
    fi
    [ -d "$STATE_DIR" ] && info "state: $STATE_DIR"
    if [ -x "$BINARY" ]; then
        say ""
        "$BINARY" --show-startup 2>&1 | sed 's/^/  /' || true
    fi
}

usage() {
    cat <<EOF
Green Curve Linux setup

Usage: $(basename "$0") <command> [--purge]

Commands:
  install     Install, start and verify the daemon; add your user to the
              '$GROUP_NAME' group and write a desktop entry.   (needs root)
  uninstall   Stop and remove the daemon and its staged binary.  (needs root)
              Settings and the group are kept unless --purge is given.
  status      Report unit, socket, group and startup-policy state.

Environment:
  GREENCURVE_BINARY   path to the greencurve executable (default: next to this script)
  GREENCURVE_USER     account to enroll instead of \$SUDO_USER
EOF
}

COMMAND=""
for arg in "$@"; do
    case "$arg" in
        install|uninstall|status) COMMAND="$arg" ;;
        --purge)                  PURGE=1 ;;
        -h|--help|help)           usage; exit 0 ;;
        *)                        die "unknown argument '$arg' (try --help)" ;;
    esac
done

case "$COMMAND" in
    install)   cmd_install ;;
    uninstall) cmd_uninstall ;;
    status)    cmd_status ;;
    *)         usage; exit 2 ;;
esac
