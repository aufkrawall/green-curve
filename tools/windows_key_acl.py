#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Owner-only Windows ACL primitives for the offline update signing key."""

import ctypes
import os


ERROR_BAD_LENGTH = 24
ERROR_INSUFFICIENT_BUFFER = 122
TOKEN_QUERY = 0x0008
TOKEN_USER_CLASS = 1
SE_FILE_OBJECT = 1
FILE_ALL_ACCESS = 0x001F01FF
DACL_SECURITY_INFORMATION = 0x00000004
OWNER_SECURITY_INFORMATION = 0x00000001
PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000
SE_DACL_PROTECTED = 0x1000
ACL_SIZE_INFORMATION_CLASS = 2
ACCESS_ALLOWED_ACE_TYPE = 0


class SidAndAttributes(ctypes.Structure):
    _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", ctypes.c_ulong)]


class TokenUser(ctypes.Structure):
    _fields_ = [("User", SidAndAttributes)]


class TrusteeW(ctypes.Structure):
    _fields_ = [
        ("pMultipleTrustee", ctypes.c_void_p),
        ("MultipleTrusteeOperation", ctypes.c_int),
        ("TrusteeForm", ctypes.c_int),
        ("TrusteeType", ctypes.c_int),
        ("ptstrName", ctypes.c_void_p),
    ]


class ExplicitAccessW(ctypes.Structure):
    _fields_ = [
        ("grfAccessPermissions", ctypes.c_uint32),
        ("grfAccessMode", ctypes.c_uint32),
        ("grfInheritance", ctypes.c_uint32),
        ("Trustee", TrusteeW),
    ]


class AclSizeInformation(ctypes.Structure):
    _fields_ = [
        ("AceCount", ctypes.c_uint32),
        ("AceBytesInUse", ctypes.c_uint32),
        ("AceBytesFree", ctypes.c_uint32),
    ]


def _libraries():
    if os.name != "nt":
        raise OSError("Windows ACL operations require Windows")
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    kernel32.GetCurrentProcess.argtypes = []
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.OpenProcessToken.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
    kernel32.OpenProcessToken.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p

    advapi32.GetTokenInformation.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong)]
    advapi32.GetTokenInformation.restype = ctypes.c_int
    advapi32.GetLengthSid.argtypes = [ctypes.c_void_p]
    advapi32.GetLengthSid.restype = ctypes.c_ulong
    advapi32.ConvertSidToStringSidW.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_wchar_p)]
    advapi32.ConvertSidToStringSidW.restype = ctypes.c_int
    advapi32.SetEntriesInAclW.argtypes = [
        ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p)]
    advapi32.SetEntriesInAclW.restype = ctypes.c_uint32
    advapi32.SetNamedSecurityInfoW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    advapi32.SetNamedSecurityInfoW.restype = ctypes.c_uint32
    advapi32.GetNamedSecurityInfoW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_int, ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p)]
    advapi32.GetNamedSecurityInfoW.restype = ctypes.c_uint32
    advapi32.GetAclInformation.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int]
    advapi32.GetAclInformation.restype = ctypes.c_int
    advapi32.GetAce.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_void_p)]
    advapi32.GetAce.restype = ctypes.c_int
    advapi32.GetSecurityDescriptorControl.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint16),
        ctypes.POINTER(ctypes.c_uint32)]
    advapi32.GetSecurityDescriptorControl.restype = ctypes.c_int
    return advapi32, kernel32


def current_user_sid():
    """Return the complete current process-token user SID as owned bytes."""
    advapi32, kernel32 = _libraries()
    token = ctypes.c_void_p()
    if not kernel32.OpenProcessToken(
            kernel32.GetCurrentProcess(), TOKEN_QUERY, ctypes.byref(token)):
        raise OSError(ctypes.get_last_error(), "OpenProcessToken failed")
    try:
        needed = ctypes.c_ulong()
        if not advapi32.GetTokenInformation(
                token, TOKEN_USER_CLASS, None, 0, ctypes.byref(needed)):
            error = ctypes.get_last_error()
            if error != ERROR_INSUFFICIENT_BUFFER:
                raise OSError(error, "GetTokenInformation sizing failed")
        # TOKEN_USER is followed by a variable-length SID. Supplying only a
        # TokenUser ctypes object here is a native buffer overflow.
        if needed.value < ctypes.sizeof(TokenUser):
            raise OSError(
                ERROR_BAD_LENGTH,
                "GetTokenInformation returned a short TOKEN_USER size")
        buffer = ctypes.create_string_buffer(needed.value)
        if not advapi32.GetTokenInformation(
                token, TOKEN_USER_CLASS, buffer, ctypes.sizeof(buffer),
                ctypes.byref(needed)):
            raise OSError(ctypes.get_last_error(), "GetTokenInformation failed")
        token_user = ctypes.cast(buffer, ctypes.POINTER(TokenUser)).contents
        sid = token_user.User.Sid
        length = advapi32.GetLengthSid(sid) if sid else 0
        if not length:
            raise OSError(ctypes.get_last_error(), "token contained no valid user SID")
        return ctypes.string_at(sid, length)
    finally:
        kernel32.CloseHandle(token)


def _sid_text(advapi32, kernel32, sid_pointer):
    text = ctypes.c_wchar_p()
    if not advapi32.ConvertSidToStringSidW(sid_pointer, ctypes.byref(text)):
        raise OSError(ctypes.get_last_error(), "ConvertSidToStringSidW failed")
    try:
        return text.value
    finally:
        kernel32.LocalFree(text)


def _owned_sid_pointer(sid):
    buffer = ctypes.create_string_buffer(sid, len(sid))
    return buffer, ctypes.cast(buffer, ctypes.c_void_p)


def harden_owner_only(path):
    """Replace the file DACL with one protected full-control owner ACE."""
    advapi32, kernel32 = _libraries()
    sid = current_user_sid()
    sid_buffer, sid_pointer = _owned_sid_pointer(sid)
    access = ExplicitAccessW()
    access.grfAccessPermissions = FILE_ALL_ACCESS
    access.grfAccessMode = 1  # GRANT_ACCESS
    access.grfInheritance = 0
    access.Trustee.TrusteeForm = 0  # TRUSTEE_IS_SID
    access.Trustee.TrusteeType = 1  # TRUSTEE_IS_USER
    access.Trustee.ptstrName = sid_pointer
    acl = ctypes.c_void_p()
    result = advapi32.SetEntriesInAclW(
        1, ctypes.byref(access), None, ctypes.byref(acl))
    if result:
        raise OSError(result, "SetEntriesInAclW failed")
    try:
        result = advapi32.SetNamedSecurityInfoW(
            path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            None, None, acl, None)
        if result:
            raise OSError(result, "SetNamedSecurityInfoW failed")
    finally:
        # Keep sid_buffer alive until SetNamedSecurityInfoW has consumed it.
        del sid_buffer
        kernel32.LocalFree(acl)
    verify_owner_only(path)


def verify_owner_only(path):
    """Require current ownership plus one protected full-control owner ACE."""
    advapi32, kernel32 = _libraries()
    expected_sid = current_user_sid()
    expected_buffer, expected_pointer = _owned_sid_pointer(expected_sid)
    expected_text = _sid_text(advapi32, kernel32, expected_pointer)
    owner = ctypes.c_void_p()
    dacl = ctypes.c_void_p()
    descriptor = ctypes.c_void_p()
    result = advapi32.GetNamedSecurityInfoW(
        path, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        ctypes.byref(owner), None, ctypes.byref(dacl), None,
        ctypes.byref(descriptor))
    if result:
        raise OSError(result, "GetNamedSecurityInfoW failed")
    try:
        if not owner or _sid_text(advapi32, kernel32, owner) != expected_text:
            raise PermissionError(f"private key is not owned by its reader: {path}")
        if not dacl:
            raise PermissionError(f"private key has no protected owner DACL: {path}")
        size = AclSizeInformation()
        if not advapi32.GetAclInformation(
                dacl, ctypes.byref(size), ctypes.sizeof(size),
                ACL_SIZE_INFORMATION_CLASS):
            raise OSError(ctypes.get_last_error(), "GetAclInformation failed")
        control = ctypes.c_uint16()
        revision = ctypes.c_uint32()
        if not advapi32.GetSecurityDescriptorControl(
                descriptor, ctypes.byref(control), ctypes.byref(revision)):
            raise OSError(
                ctypes.get_last_error(), "GetSecurityDescriptorControl failed")
        if size.AceCount != 1 or not (control.value & SE_DACL_PROTECTED):
            raise PermissionError(f"private key has a non-owner-only ACL: {path}")
        ace = ctypes.c_void_p()
        if not advapi32.GetAce(dacl, 0, ctypes.byref(ace)):
            raise OSError(ctypes.get_last_error(), "GetAce failed")
        address = ace.value
        header = ctypes.string_at(address, 4)
        mask = ctypes.c_uint32.from_address(address + 4).value
        ace_sid = ctypes.c_void_p(address + 8)
        if (header[0] != ACCESS_ALLOWED_ACE_TYPE or mask != FILE_ALL_ACCESS or
                _sid_text(advapi32, kernel32, ace_sid) != expected_text):
            raise PermissionError(f"private key ACL is not one owner grant: {path}")
    finally:
        del expected_buffer
        kernel32.LocalFree(descriptor)
