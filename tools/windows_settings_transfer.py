# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Carry the service's applied settings across a service restart from tooling.

A clean stop of the Windows service returns the GPU to stock (it resets the
intent it owns), and a started service holds no applied settings: startup
never replays.  Setup therefore exports the active settings before it stops
the service and applies them after the new one runs (source/installer_apply.cpp).
Any tool that restarts the service must do the same, or the user's profile is
silently gone -- the release simulation's teardown did exactly that.

This is the same pair of CLI verbs, the same exit-code contract
(source/settings_transfer_exit_policy.h) and the same administrator-only
scratch folder directly under Program Files that setup uses, so the elevated
apply never reads a file a medium-integrity process could have replaced.
tests: simulate_release_tests.py.
"""

import ctypes
import os
import secrets
import subprocess
from pathlib import Path

GUI_EXE = "greencurve.exe"  # installer_common.h GC_SETUP_GUI_EXE
# settings_transfer_exit_policy.h GC_SETTINGS_TRANSFER_NO_ACTIVE_EXIT_CODE
NO_ACTIVE_EXIT_CODE = 4
# installer_apply.cpp GC_APP_EXPORT_TIMEOUT_MS / GC_APP_REAPPLY_TIMEOUT_MS
EXPORT_TIMEOUT_S = 20
REAPPLY_TIMEOUT_S = 180
# installer_util.cpp gc_create_private_temp_directory()
SCRATCH_SDDL = "D:P(A;;FA;;;SY)(A;;FA;;;BA)"
SNAPSHOT_NAME = "greencurve-teardown-settings.ini"

CAPTURE_SAVED = "saved"
CAPTURE_NONE_ACTIVE = "none-active"
CAPTURE_FAILED = "failed"


def capture_result(ran, exit_code, snapshot_readable):
    """gc_settings_capture_attempt_result(), value for value."""
    if ran and exit_code == 0 and snapshot_readable:
        return CAPTURE_SAVED
    if ran and exit_code == NO_ACTIVE_EXIT_CODE and not snapshot_readable:
        return CAPTURE_NONE_ACTIVE
    return CAPTURE_FAILED


class _Guid(ctypes.Structure):
    _fields_ = [("data1", ctypes.c_uint32), ("data2", ctypes.c_uint16),
                ("data3", ctypes.c_uint16), ("data4", ctypes.c_ubyte * 8)]


# FOLDERID_ProgramFiles {905e63b6-c1bf-494e-b29c-65b732d3d21a}
_FOLDERID_PROGRAM_FILES = _Guid(0x905E63B6, 0xC1BF, 0x494E,
                                (ctypes.c_ubyte * 8)(0xB2, 0x9C, 0x65, 0xB7,
                                                     0x32, 0xD3, 0xD2, 0x1A))


class _SecurityAttributes(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint32), ("descriptor", ctypes.c_void_p),
                ("inherit", ctypes.c_int)]


def _program_files():
    """The known folder itself, not an environment variable the user controls."""
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    ole = ctypes.WinDLL("ole32")
    shell.SHGetKnownFolderPath.argtypes = [ctypes.POINTER(_Guid), ctypes.c_uint32,
                                           ctypes.c_void_p, ctypes.POINTER(ctypes.c_wchar_p)]
    shell.SHGetKnownFolderPath.restype = ctypes.c_long
    ole.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    path = ctypes.c_wchar_p()
    result = shell.SHGetKnownFolderPath(ctypes.byref(_FOLDERID_PROGRAM_FILES), 0, None,
                                        ctypes.byref(path))
    try:
        if result != 0 or not path.value:
            raise OSError(f"SHGetKnownFolderPath(ProgramFiles) failed (0x{result & 0xFFFFFFFF:08x})")
        return Path(path.value)
    finally:
        ole.CoTaskMemFree(ctypes.cast(path, ctypes.c_void_p))


def create_private_scratch():
    """A fresh folder only SYSTEM and Administrators can touch, never adopted."""
    if os.name != "nt":
        raise OSError("the settings transfer is only implemented on Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = ctypes.c_int
    kernel.CreateDirectoryW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(_SecurityAttributes)]
    kernel.CreateDirectoryW.restype = ctypes.c_int
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    root = _program_files()
    descriptor = ctypes.c_void_p()
    if not security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            SCRATCH_SDDL, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        attributes = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), descriptor, 0)
        for _attempt in range(8):
            folder = root / f"greencurve-teardown-{secrets.token_hex(8)}"
            if kernel.CreateDirectoryW(str(folder), ctypes.byref(attributes)):
                break
            error = ctypes.get_last_error()
            if error != 183:  # ERROR_ALREADY_EXISTS: pick another name, never adopt
                raise ctypes.WinError(error)
        else:
            raise OSError("could not create a private scratch folder")
    finally:
        kernel.LocalFree(descriptor)
    if folder.is_symlink() or not folder.is_dir() or os.path.isjunction(folder):
        folder.rmdir()
        raise OSError(f"{folder} is not a plain folder")
    return folder


def _tail(completed):
    text = ((completed.stdout or "") + (completed.stderr or "")).strip()
    return text.splitlines()[-1][:200] if text else ""


class SettingsTransfer:
    """Export before the stop, apply after the start, discard either way."""

    def __init__(self, gui_exe, run=subprocess.run, make_scratch=create_private_scratch):
        self.gui_exe = Path(gui_exe) if gui_exe else None
        self._run = run
        self._make_scratch = make_scratch
        self.scratch = None
        self.snapshot = None

    def _invoke(self, verb, timeout_s):
        """(ran, exit code, last output line)."""
        try:
            completed = self._run([str(self.gui_exe), verb, str(self.snapshot)],
                                  capture_output=True, text=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return False, None, f"timed out after {timeout_s} s (terminated)"
        except OSError as exc:
            return False, None, str(exc)
        return True, completed.returncode, _tail(completed)

    def capture(self):
        """(CAPTURE_*, detail).  Only SAVED leaves a snapshot to apply."""
        if not self.gui_exe or not self.gui_exe.is_file():
            return CAPTURE_FAILED, f"no {GUI_EXE} at {self.gui_exe}"
        try:
            self.scratch = self._make_scratch()
        except OSError as exc:
            return CAPTURE_FAILED, f"no private scratch folder ({exc})"
        self.snapshot = self.scratch / SNAPSHOT_NAME
        ran, code, detail = self._invoke("--export-active-settings", EXPORT_TIMEOUT_S)
        result = capture_result(ran, code, self.snapshot.is_file())
        if result != CAPTURE_SAVED:
            self.discard()
        return result, f"exit {code}: {detail}" if detail else f"exit {code}"

    def reapply(self):
        """(applied, detail).  Consumes the snapshot."""
        if not self.snapshot:
            return False, "nothing was saved"
        try:
            ran, code, detail = self._invoke("--apply-settings-file", REAPPLY_TIMEOUT_S)
        finally:
            self.discard()
        return ran and code == 0, f"exit {code}: {detail}" if detail else f"exit {code}"

    def discard(self):
        if self.snapshot:
            self.snapshot.unlink(missing_ok=True)
            self.snapshot = None
        if self.scratch:
            try:
                self.scratch.rmdir()
            except OSError:
                pass
            self.scratch = None
