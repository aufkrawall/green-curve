#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Guards for tools/simulate_release.py.

A release simulation builds and publishes installers that look exactly like
real releases.  These helpers keep that from ever reaching a real installation
or the real channel:

- simulated builds trust ONLY a separate simulation key (patched into the
  isolated worktree like the repository constants), and signing refuses any
  key a release build trusts or the release key's own path;
- no step may target the production repository;
- the installed client's [updates] highest_seen_version is recorded before a
  simulation and restored afterwards, with the service that owns it stopped,
  and from an elevated child when teardown itself is not elevated.
"""

import ctypes
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import update_signing
import windows_service_control
import windows_settings_transfer
from windows_config_lock import config_storage_lock

SIMULATION_KEY_NAME = "simulation-update-signing-key.txt"
RELEASE_KEY_ARRAYS = ("GC_UPDATE_PUBLIC_KEY_ACTIVE", "GC_UPDATE_PUBLIC_KEY_NEXT")
SEEN_SECTION = "updates"
SEEN_KEY = "highest_seen_version"
SIMULATE_SCRIPT = SCRIPT_DIR / "simulate_release.py"
_REPO_RE = re.compile(r"([A-Za-z0-9_-]+)/([A-Za-z0-9_.-]+)")
# The value is whatever bytes the file holds.  A hand-edited or damaged value
# must round-trip through the JSON record and back without aborting init or
# teardown, so undecodable bytes are carried as surrogate escapes.
_VALUE_CODEC = ("utf-8", "surrogateescape")


def log(msg, prefix="[simulate-release]"):
    # A value carried as surrogate escapes (see _VALUE_CODEC) holds lone
    # surrogates; a strict stdout (UTF-8 redirected to a file) would raise
    # instead of logging it, aborting the init or teardown being logged.
    line = f"{prefix} {msg}"
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    print(line.encode(encoding, "backslashreplace").decode(encoding), flush=True)


def get_default_key_path():
    """Locate the SIMULATION update signing key.

    Deliberately a different file from the release key: a simulated build is
    signed with a key no shipped build trusts, so nothing published by a
    simulation can ever verify on a real installation.
    """
    userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if not userprofile:
        return None
    return Path(userprofile) / ".greencurve-keys" / SIMULATION_KEY_NAME


def get_release_key_path():
    """The release key's conventional location, which a simulation refuses."""
    userprofile = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if not userprofile:
        return None
    return Path(userprofile) / ".greencurve-keys" / "update-signing-key.txt"


def _key_array_hex(text, name):
    m = re.search(name + r"\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))).hex().lower()


def get_release_pubkeys(root=ROOT_DIR):
    """Every public key the repository's own builds trust (ACTIVE and NEXT)."""
    header = Path(root) / "source" / "update_verify_keys.h"
    text = header.read_text(encoding="utf-8")
    keys = {_key_array_hex(text, name) for name in RELEASE_KEY_ARRAYS}
    keys.discard(None)
    if not keys:
        raise ValueError(f"no update public keys found in {header}")
    return keys


def get_production_repo(root=ROOT_DIR):
    """owner/repo the repository's own builds fetch updates from."""
    policy = Path(root) / "source" / "update_url_policy.h"
    text = policy.read_text(encoding="utf-8")
    owner = re.search(r'#define GC_UPDATE_REPO_OWNER "([^"]+)"', text)
    name = re.search(r'#define GC_UPDATE_REPO_NAME\s+"([^"]+)"', text)
    if not owner or not name:
        raise ValueError(f"could not read the update repository from {policy}")
    return f"{owner.group(1)}/{name.group(1)}"


def require_simulation_repo(repo, root=ROOT_DIR):
    """Refuse to point any simulation step at the production repository."""
    if not _REPO_RE.fullmatch((repo or "").strip()):
        raise ValueError(f"invalid repository format '{repo}'; expected owner/repo")
    production = get_production_repo(root)
    if repo.strip().lower() == production.lower():
        raise RuntimeError(
            f"refusing to run a simulation against the production repository {production}; "
            "pass a dedicated test repository with --repo")
    return repo.strip()


def simulation_pubkey_hex(key_path, root=ROOT_DIR):
    """Public key of the simulation key; refuses any key a release build trusts."""
    key_path = Path(key_path)
    release_path = get_release_key_path()
    if release_path is not None:
        try:
            if key_path.resolve() == release_path.resolve():
                raise RuntimeError(
                    f"refusing the release signing key path {key_path}; simulations sign with "
                    f"their own key (default: {get_default_key_path()})")
        except OSError:
            pass
    private_key = update_signing.read_private_key(str(key_path))
    point = update_signing.public_key_from_private(private_key)
    pubkey_hex = update_signing.public_key_bytes(point).hex().lower()
    if pubkey_hex in get_release_pubkeys(root):
        raise RuntimeError(
            "refusing to sign a simulation with a key that release builds trust; "
            "generate a separate simulation key (init creates one)")
    return pubkey_hex


def ensure_simulation_key(key_path, root=ROOT_DIR, dry_run=False):
    """Create the simulation key (owner-only) if it is missing; return its public key."""
    key_path = Path(key_path)
    if not key_path.exists():
        if dry_run:
            log(f"[DRY-RUN] would create a simulation signing key at {key_path}")
            return None
        key_path.parent.mkdir(parents=True, exist_ok=True)
        update_signing.write_private_key(str(key_path), update_signing.generate_private_key())
        log(f"created a simulation signing key at {key_path}")
    return simulation_pubkey_hex(key_path, root)


def patch_update_keys(source_dir, pubkey_hex):
    """Make a worktree's build trust ONLY the simulation key (ACTIVE and NEXT).

    Like patch_update_repo, this edits an isolated worktree and is never
    committed.  Both slots are replaced so a simulated client cannot accept a
    manifest signed with a release key either.
    """
    raw = bytes.fromhex(pubkey_hex)
    if len(raw) != 64:
        raise ValueError("simulation public key must be 64 bytes")
    header = Path(source_dir) / "source" / "update_verify_keys.h"
    if not header.exists():
        raise FileNotFoundError(f"missing update_verify_keys.h at {header}")
    text = header.read_text(encoding="utf-8")
    body = "\n" + "\n".join(
        "    " + ", ".join(f"0x{b:02X}" for b in raw[i:i + 8]) + ","
        for i in range(0, 64, 8)) + "\n"
    for name in RELEASE_KEY_ARRAYS:
        text, count = re.subn("(" + name + r"\[[^\]]+\]\s*=\s*\{)(.*?)(\};)",
                              lambda m: m.group(1) + body + m.group(3), text,
                              count=1, flags=re.DOTALL)
        if count != 1:
            raise RuntimeError(f"failed to patch {name} in {header}")
    for name in RELEASE_KEY_ARRAYS:
        if _key_array_hex(text, name) != pubkey_hex:
            raise RuntimeError(f"{name} did not read back as the simulation key")
    header.write_text(text, encoding="utf-8")
    log(f"patched {header.name} -> simulation key {pubkey_hex[:16]}...")


# ---------------------------------------------------------------------------
# The installed client's [updates] highest_seen_version
# ---------------------------------------------------------------------------
#
# Every check the simulated client makes raises the machine-wide high-water
# mark to the simulated version.  Left there, the next real release (numbered
# below the simulated one) would be reported as the channel going backwards.
# init records the value before anything is published and teardown restores it.
#
# The restore only sticks while the service is stopped: the service loads the
# value once at start and writes its in-memory copy back on every settings
# save (a failed check included).  The restore used to edit the file under a
# running service and print "restart it before its next check"; a teardown run
# from a normal prompt only printed an elevated command.  Both were easy to
# miss, and the real build then warned about an older release on every check.
# The restart in turn must carry the applied settings across it, as setup does
# (windows_settings_transfer.py): a clean stop resets the GPU to stock.

def machine_config_path():
    program_data = os.environ.get("ProgramData") or r"C:\ProgramData"
    return Path(program_data) / "Green Curve" / "shared-profiles.ini"


def default_state_path():
    return ROOT_DIR / "dist" / "test-simulation" / "pre-simulation-state.json"


def _updates_section_bounds(lines):
    """(header index, body start, body end) for [updates], or None."""
    header = None
    for index, line in enumerate(lines):
        stripped = line.strip(b" \t\r\n")
        if stripped.startswith(b"[") and stripped.endswith(b"]"):
            if header is not None:
                return header, header + 1, index
            if stripped[1:-1].strip().lower() == SEEN_SECTION.encode("ascii"):
                header = index
    return (header, header + 1, len(lines)) if header is not None else None


def _updates_section_span(lines):
    """(start, end) line indices of the [updates] body, or None."""
    bounds = _updates_section_bounds(lines)
    return (bounds[1], bounds[2]) if bounds is not None else None


def _ini_lines(data):
    # str.splitlines() also splits ANSI/UTF-8 bytes such as 0x85. Retain the
    # actual CR/LF endings, including mixed endings and an unterminated EOF.
    return [line for line in re.findall(rb"[^\r\n]*(?:\r\n|\r|\n|$)", data) if line]


def _empty_highest_seen_layout(file_present=False):
    return {
        "file_present": file_present,
        "file_ended_with_newline": False,
        "updates_present": False,
        "highest_seen_present": False,
        "updates_nonblank_lines": 0,
    }


def _highest_seen_state_from_bytes(data):
    """Value plus the minimal layout facts needed to reverse an absent-key edit."""
    layout = _empty_highest_seen_layout(file_present=True)
    layout["file_ended_with_newline"] = bool(re.search(rb"(?:\r\n|\r|\n)$", data))
    lines = _ini_lines(data)
    bounds = _updates_section_bounds(lines)
    if bounds is None:
        return None, layout
    _, body_start, body_end = bounds
    layout["updates_present"] = True
    layout["updates_nonblank_lines"] = sum(
        1 for line in lines[body_start:body_end] if line.strip())
    for line in lines[body_start:body_end]:
        key, sep, value = line.partition(b"=")
        if sep and key.strip().lower() == SEEN_KEY.encode("ascii"):
            layout["highest_seen_present"] = True
            return value.strip().decode(*_VALUE_CODEC), layout
    return None, layout


def _read_highest_seen_locked(ini_path):
    if not ini_path.exists():
        return None, _empty_highest_seen_layout()
    return _highest_seen_state_from_bytes(ini_path.read_bytes())


def read_highest_seen(ini_path):
    """The recorded value, or None when the file or key is absent."""
    ini_path = Path(ini_path)
    with config_storage_lock():
        value, _layout = _read_highest_seen_locked(ini_path)
    return value


def _strip_one_final_line_ending(lines):
    if lines and lines[-1].endswith((b"\r", b"\n")):
        lines[-1] = re.sub(rb"(?:\r\n|\r|\n)$", b"", lines[-1], count=1)


def _cleanup_absent_key_artifacts(lines, layout):
    """Remove only structure this editor knows it added while the key was absent.

    Existing [updates] entries written by the simulation are retained.  If an
    absent section became non-empty for any other reason, only the seen-version
    key is removed elsewhere; this cleanup becomes a no-op.
    """
    if not layout or layout.get("highest_seen_present", True):
        return
    bounds = _updates_section_bounds(lines)
    if bounds is None:
        return
    header, body_start, body_end = bounds
    updates_at_eof = body_end == len(lines)
    nonblank = sum(1 for line in lines[body_start:body_end] if line.strip())
    originally_present = bool(layout.get("updates_present"))
    can_restore_final_termination = False
    if not originally_present:
        if nonblank:
            return
        # The writer appends a missing section at EOF.  Remove the header, its
        # body and the separator blank line it introduced; retain anything the
        # simulation appended after it.
        del lines[header:body_end]
        if header > 0 and not lines[header - 1].strip():
            del lines[header - 1]
        can_restore_final_termination = updates_at_eof
    else:
        original_nonblank = int(layout.get("updates_nonblank_lines", 0))
        if nonblank != original_nonblank:
            return
        can_restore_final_termination = updates_at_eof
    if can_restore_final_termination and not layout.get("file_ended_with_newline", True):
        # Inserting a line after an unterminated final line had to add an EOL.
        _strip_one_final_line_ending(lines)


def _absent_key_cleanup_needed_locked(ini_path, layout):
    if not ini_path.exists():
        return False
    data = ini_path.read_bytes()
    lines = _ini_lines(data)
    _cleanup_absent_key_artifacts(lines, layout)
    out = b"".join(lines)
    if not out and not layout.get("file_present", True):
        return True
    return out != data


def set_highest_seen(ini_path, value, restore_layout=None):
    """Rewrite one key IN PLACE (same file object, so its protected DACL and
    owner are kept); value None removes the key.  Returns True when changed.

    `restore_layout` is the layout recorded before a simulation.  It is used
    only when removing a key that was absent then, so empty section scaffolding
    and an artificially added final newline can be removed again.
    """
    # Read only after the shared mutex is held: another writer may finish
    # while this caller waits, and its unrelated edits must survive the restore.
    with config_storage_lock():
        return _set_highest_seen_locked(Path(ini_path), value, restore_layout)


def _set_highest_seen_locked(ini_path, value, restore_layout=None):
    data = ini_path.read_bytes()
    match = re.search(rb"\r\n|\r|\n", data)
    newline = match.group() if match else b"\n"
    lines = _ini_lines(data)
    span = _updates_section_span(lines)
    replacement = None if value is None else f"{SEEN_KEY}={value}".encode(*_VALUE_CODEC)
    found = False
    if span is not None:
        for index in range(span[0], span[1]):
            key, sep, _ = lines[index].partition(b"=")
            if sep and key.strip().lower() == SEEN_KEY.encode("ascii"):
                ending = re.search(rb"(?:\r\n|\r|\n)$", lines[index])
                if replacement is None:
                    del lines[index]
                else:
                    new_line = replacement + (ending.group() if ending else b"")
                    if lines[index] == new_line:
                        return False
                    lines[index] = new_line
                found = True
                break
        if not found and replacement is not None:
            insert_at = span[1]
            while insert_at > span[0] and not lines[insert_at - 1].strip():
                insert_at -= 1
            if insert_at and not lines[insert_at - 1].endswith((b"\r", b"\n")):
                lines[insert_at - 1] += newline
            lines.insert(insert_at, replacement + newline)
    elif replacement is not None:
        if lines and not lines[-1].endswith((b"\r", b"\n")):
            lines[-1] += newline
        lines += [newline, b"[updates]" + newline, replacement + newline]
    if replacement is None:
        _cleanup_absent_key_artifacts(lines, restore_layout)
    out = b"".join(lines)
    remove_file = (replacement is None and restore_layout is not None and
                   not restore_layout.get("file_present", True) and
                   not out and ini_path.exists())
    if out == data and not remove_file:
        return False
    if out != data:
        with open(ini_path, "r+b") as handle:
            handle.seek(0)
            handle.write(out)
            handle.truncate()
    if remove_file:
        ini_path.unlink()
    return True


def record_pre_simulation_state(state_path=None, ini_path=None):
    """Remember highest_seen_version before the first simulated publish.

    Never overwrites an existing record: a second init during the same
    simulation would otherwise record the already-raised value.
    """
    state_path = Path(state_path or default_state_path())
    ini_path = Path(ini_path or machine_config_path())
    if state_path.exists():
        log(f"pre-simulation state already recorded in {state_path}; keeping it "
            "(an earlier teardown has not finished its seen-version restore)")
        return json.loads(state_path.read_text(encoding="utf-8"))
    with config_storage_lock():
        value, layout = _read_highest_seen_locked(ini_path)
    state = {
        "highest_seen_version": value,
        "machine_config": str(ini_path),
        "highest_seen_layout": layout,
    }
    state_path.parent.mkdir(parents=True, exist_ok=True)
    state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    log(f"recorded pre-simulation highest_seen_version="
        f"{state['highest_seen_version'] or '<absent>'} in {state_path}")
    return state


def is_elevated():
    if os.name != "nt":
        return hasattr(os, "geteuid") and os.geteuid() == 0
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        return False


def restore_command(state_path, override=None):
    """The exact command an elevated prompt must run to finish the restore."""
    command = (f'"{sys.executable}" "{SIMULATE_SCRIPT}" teardown '
               f'--restore-seen-only --state "{Path(state_path).resolve()}"')
    if override is not None:
        command += f' --seen-version "{override}"'
    return command


def service_controller():
    """The service that owns the machine configuration (replaced by the tests)."""
    return windows_service_control.default_controller()


def settings_transfer_for(service):
    """Carries the service's applied settings across its restart (replaced by the tests)."""
    directory = service.install_directory()
    return windows_settings_transfer.SettingsTransfer(
        directory / windows_settings_transfer.GUI_EXE if directory else None)


def _write_with_service_stopped(ini_path, target, layout, service):
    """Save the applied settings, stop the owning service, write and verify,
    start it again and re-apply what was saved.

    A clean stop returns the GPU to stock and a started service applies
    nothing on its own, so a bare restart silently dropped the user's profile
    (setup carries it across its own stop the same way).  Returns the steps
    taken.  Raises when the value cannot be made to stick or the settings did
    not come back: a service whose settings could not be saved, or that does
    not stop, is never written under; a failed write still restarts the
    service and re-applies the settings.
    """
    state = service.query_state()
    stop = state is not None and state != windows_service_control.STATE_STOPPED
    # A service already on its way down is waited for, not brought back: its
    # settings are being reset by that stop already.
    restart = stop and state != windows_service_control.STATE_STOP_PENDING
    steps = []
    transfer = None
    if restart:
        transfer = settings_transfer_for(service)
        captured, detail = transfer.capture()
        log(f"settings transfer: export {captured} ({detail})")
        if captured == windows_settings_transfer.CAPTURE_FAILED:
            raise RuntimeError(
                f"could not save the settings {service.name} has applied ({detail}); "
                "the service was not stopped, because stopping it returns the GPU to "
                "stock. Retry, or Reset to stock in Green Curve first if nothing needs "
                "keeping")
        if captured == windows_settings_transfer.CAPTURE_NONE_ACTIVE:
            transfer = None
            steps.append("no applied settings to carry over")
        else:
            steps.append("saved the applied settings")
    try:
        if stop:
            service.stop()
            steps.append(f"stopped {service.name} "
                         f"(was {windows_service_control.state_name(state)})")
        error = None
        try:
            set_highest_seen(ini_path, target, layout if target is None else None)
            current = read_highest_seen(ini_path)
            if current != target:
                raise RuntimeError(f"{SEEN_KEY} reads back as {current or '<absent>'}, "
                                   f"not {target or '<absent>'}")
        except (OSError, RuntimeError, ValueError) as exc:
            error = exc
        lost = "; the saved settings were NOT re-applied: apply your profile in " \
               "Green Curve" if transfer else ""
        if restart:
            try:
                service.start()
            except (OSError, RuntimeError) as exc:
                outcome = f"the restore failed ({error}) and" if error else \
                    f"{SEEN_KEY} was restored, but"
                raise RuntimeError(f"{outcome} {service.name} did not start again: {exc}. "
                                   f"Start it with: sc start {service.name}{lost}") from exc
            steps.append(f"started {service.name}")
        if transfer:
            applied, detail = transfer.reapply()
            log(f"settings transfer: re-apply {'ok' if applied else 'FAILED'} ({detail})")
            if not applied:
                outcome = f"the restore failed ({error}), and" if error else \
                    f"{SEEN_KEY} was restored, but"
                raise RuntimeError(f"{outcome} the saved settings were NOT re-applied "
                                   f"({detail}); apply your profile in Green Curve")
            steps.append("re-applied the saved settings")
        if error:
            raise error
        return steps
    finally:
        if transfer:
            transfer.discard()


def restore_highest_seen(state_path=None, ini_path=None, override=None, elevated=None,
                         service=None, elevate=None):
    """Put highest_seen_version back; returns (restored, message).

    Elevated, the write happens with the owning service stopped.  Not
    elevated, `elevate(state_path, override)` runs the same restore in an
    elevated child and returns its result; without it the message carries
    the exact command to run.
    """
    state_path = Path(state_path or default_state_path())
    layout = None
    if override is not None:
        target = override or None
    elif state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        target = state.get("highest_seen_version")
        layout = state.get("highest_seen_layout")
    else:
        return False, (f"no pre-simulation record at {state_path}; pass --seen-version <v> "
                       "(empty string removes the key) to restore by hand")
    ini_path = Path(ini_path or machine_config_path())
    current = read_highest_seen(ini_path)
    cleanup_possible = target is None and layout is not None and ini_path.exists()
    cleanup_needed = False
    if cleanup_possible:
        with config_storage_lock():
            cleanup_needed = _absent_key_cleanup_needed_locked(ini_path, layout)
    if current == target and not cleanup_needed:
        return True, f"highest_seen_version already {target or '<absent>'}; nothing to restore"
    if current == target:
        change = ("highest_seen_version is already absent, but simulation-only "
                  "configuration scaffolding remains")
    else:
        change = (f"highest_seen_version is {current or '<absent>'}, "
                  f"recorded {target or '<absent>'}")
    if elevated is None:
        elevated = is_elevated()
    if not elevated:
        if elevate is not None:
            return elevate(state_path, override)
        return False, (f"{change}; restoring it needs an elevated prompt. Run:\n  "
                       f"{restore_command(state_path, override)}")
    if not ini_path.exists():
        return False, f"{ini_path} does not exist; nothing to restore"
    service = service or service_controller()
    try:
        steps = _write_with_service_stopped(ini_path, target, layout, service)
    except (OSError, RuntimeError, ValueError) as exc:
        return False, f"{change}; the restore did not complete: {exc}"
    done = (f"restored highest_seen_version {current or '<absent>'} -> {target or '<absent>'}"
            if current != target else "removed simulation-only configuration scaffolding")
    if steps:
        return True, f"{done}; {', '.join(steps)}"
    return True, f"{done}; {service.name} was not running, so its next start loads it"


# ---------------------------------------------------------------------------
# Elevated restore child
# ---------------------------------------------------------------------------
#
# The machine configuration and the service need an elevated token, and a
# teardown run from a normal prompt used to stop at printing a command.  It now
# asks for elevation once (UAC) and runs `teardown --restore-seen-only` as an
# elevated child.  The child's console is its own, so it reports through a
# JSON file next to the pre-simulation record.

RESTORE_REPORT_NAME = "teardown-restore-report.json"
# The child exports the settings (20 s), stops and starts the service (each
# wait bounded at 60 s by the SCM protocol), waits up to 5 s for the
# configuration mutex and re-applies the settings (180 s, setup's bound).
ELEVATED_RESTORE_WAIT_MS = 360000


def elevated_restore_args(state_path, report_path, override=None):
    args = [str(SIMULATE_SCRIPT), "teardown", "--restore-seen-only", "--no-elevate",
            "--state", str(Path(state_path).resolve()), "--report", str(report_path)]
    if override is not None:
        args += ["--seen-version", override]
    return args


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint32), ("fMask", ctypes.c_ulong),
                ("hwnd", ctypes.c_void_p), ("lpVerb", ctypes.c_wchar_p),
                ("lpFile", ctypes.c_wchar_p), ("lpParameters", ctypes.c_wchar_p),
                ("lpDirectory", ctypes.c_wchar_p), ("nShow", ctypes.c_int),
                ("hInstApp", ctypes.c_void_p), ("lpIDList", ctypes.c_void_p),
                ("lpClass", ctypes.c_wchar_p), ("hkeyClass", ctypes.c_void_p),
                ("dwHotKey", ctypes.c_uint32), ("hIconOrMonitor", ctypes.c_void_p),
                ("hProcess", ctypes.c_void_p)]


def _shell_execute_elevated(executable, args, cwd):
    """Run `executable args` elevated, wait for it, and return its exit code.

    Raises OSError when elevation is declined or the launch fails, and
    TimeoutError when the child outlives ELEVATED_RESTORE_WAIT_MS.
    """
    if os.name != "nt":
        raise OSError("elevated relaunch is only implemented on Windows")
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    shell.ShellExecuteExW.argtypes = [ctypes.POINTER(_ShellExecuteInfo)]
    shell.ShellExecuteExW.restype = ctypes.c_int
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    kernel.GetExitCodeProcess.restype = ctypes.c_int
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    info = _ShellExecuteInfo()
    info.cbSize = ctypes.sizeof(info)
    info.fMask = 0x00000040 | 0x00000100  # SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC
    info.lpVerb = "runas"
    info.lpFile = str(executable)
    info.lpParameters = subprocess.list2cmdline([str(a) for a in args])
    info.lpDirectory = str(cwd)
    info.nShow = 7  # SW_SHOWMINNOACTIVE: visible in the taskbar, never steals focus
    if not shell.ShellExecuteExW(ctypes.byref(info)):
        error = ctypes.get_last_error()
        if error == 1223:  # ERROR_CANCELLED
            raise OSError(error, "elevation was declined")
        raise ctypes.WinError(error)
    if not info.hProcess:
        raise OSError("the elevated child started without a process handle")
    try:
        wait = kernel.WaitForSingleObject(info.hProcess, ELEVATED_RESTORE_WAIT_MS)
        if wait == 258:  # WAIT_TIMEOUT
            raise TimeoutError(f"the elevated restore was still running after "
                               f"{ELEVATED_RESTORE_WAIT_MS // 1000} s")
        if wait != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = ctypes.c_uint32()
        if not kernel.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return code.value
    finally:
        kernel.CloseHandle(info.hProcess)


def run_elevated_restore(state_path, override=None, launch=None):
    """Run the restore in an elevated child; returns its (restored, message)."""
    state_path = Path(state_path)
    report = state_path.with_name(RESTORE_REPORT_NAME)
    # An override restore may run with no record (and so no folder) at all.
    report.parent.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)
    command = restore_command(state_path, override)
    log(f"requesting elevation (UAC) to restore {SEEN_KEY} with the Green Curve "
        "service stopped...")
    try:
        exit_code = (launch or _shell_execute_elevated)(
            sys.executable, elevated_restore_args(state_path, report, override), ROOT_DIR)
    except OSError as exc:
        return False, (f"the elevated restore did not run ({exc}). "
                       f"Run from an elevated prompt:\n  {command}")
    try:
        result = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        result = None
    finally:
        report.unlink(missing_ok=True)
    if not isinstance(result, dict):
        return False, (f"the elevated restore exited with code {exit_code} and left no "
                       f"report. Run from an elevated prompt:\n  {command}")
    message = f"elevated: {result.get('message', '')}"
    if exit_code != 0 or result.get("restored") is not True:
        return False, f"{message}\nRun from an elevated prompt:\n  {command}"
    return True, message
