# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""The config_utils.cpp cross-session storage mutex for Windows tooling."""

import ctypes
import os
from contextlib import contextmanager

# These are the same contract as source/config_utils.cpp. The explicit medium
# integrity label lets an unelevated GUI use a mutex first created by this tool.
CONFIG_MUTEX_NAME = "Global\\GreenCurveConfigMutex-v2"
CONFIG_MUTEX_ACCESS = 0x00100001  # SYNCHRONIZE | MUTEX_MODIFY_STATE
CONFIG_MUTEX_SDDL = ("D:P(A;;0x00100001;;;SY)(A;;0x00100001;;;BA)"
                     "(A;;0x00100001;;;AU)S:(ML;;NW;;;ME)")


class SecurityAttributes(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint32),
                ("descriptor", ctypes.c_void_p), ("inherit", ctypes.c_int)]


def _apis():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_void_p]
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = ctypes.c_int
    kernel.CreateMutexExW.argtypes = [ctypes.POINTER(SecurityAttributes),
                                    ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32]
    kernel.CreateMutexExW.restype = ctypes.c_void_p
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    kernel.ReleaseMutex.argtypes = [ctypes.c_void_p]
    kernel.ReleaseMutex.restype = ctypes.c_int
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    return kernel, security


@contextmanager
def config_storage_lock(name=CONFIG_MUTEX_NAME, timeout_ms=5000):
    """Hold the app's mutex, or fail before any configuration read or write.

    Non-Windows tests inject a lock for byte-edit fixtures; a real Windows
    machine configuration must never be edited under an unrelated host lock.
    """
    if os.name != "nt":
        raise RuntimeError("Windows configuration storage requires its native mutex")
    kernel, security = _apis()
    descriptor = ctypes.c_void_p()
    if not security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            CONFIG_MUTEX_SDDL, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        attributes = SecurityAttributes(ctypes.sizeof(SecurityAttributes), descriptor, False)
        mutex = kernel.CreateMutexExW(ctypes.byref(attributes), name, 0, CONFIG_MUTEX_ACCESS)
        error = ctypes.get_last_error()
    finally:
        kernel.LocalFree(descriptor)
    if not mutex:
        raise OSError(error, "Configuration mutex creation/open failed")
    acquired = False
    try:
        result = kernel.WaitForSingleObject(mutex, timeout_ms)
        if result == 258:  # WAIT_TIMEOUT
            raise TimeoutError("Configuration mutex wait timed out; no settings were changed")
        if result not in (0, 0x80):  # WAIT_OBJECT_0 / WAIT_ABANDONED
            raise OSError(ctypes.get_last_error(), "Configuration mutex wait failed")
        acquired = True
        yield
    finally:
        try:
            if acquired and not kernel.ReleaseMutex(mutex):
                raise OSError(ctypes.get_last_error(), "Configuration mutex release failed")
        finally:
            kernel.CloseHandle(mutex)
