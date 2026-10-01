# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Release-tool input rules matching the native updater's manifest policy."""
import re

# Keep in sync with source/update_{version,manifest}_policy.h.
MAX_MANIFEST_BYTES = 4096
MAX_LINE_BYTES = 512
MAX_ASSET_BYTES = 268435456
ARCHES = ("x64", "arm64")
_COMPONENT = r"(?:0|[1-9][0-9]{0,5})"
_VERSION = re.compile(rf"{_COMPONENT}\.{_COMPONENT}(?:\.{_COMPONENT})?")


def validate_version(version):
    if not isinstance(version, str) or not _VERSION.fullmatch(version):
        raise ValueError("version must be MAJOR.MINOR[.PATCH], with no leading zeros "
                         "and each component at most 999999")
    return version


def parse_manifest(payload):
    """Validate without normalizing the signed bytes, returning exact field values."""
    if not isinstance(payload, bytes) or not 0 < len(payload) <= MAX_MANIFEST_BYTES:
        raise ValueError("manifest size is out of range")
    allowed = {"format", "version", "min_from"} | {
        f"{arch}_{field}" for arch in ARCHES for field in ("file", "size", "sha256")}
    entries = {}
    for raw in payload.split(b"\n"):
        # The native parser strips a trailing CR, accepts blank/comment lines,
        # and applies the line-size bound only to fields.
        raw = raw.removesuffix(b"\r")
        if not raw or raw.startswith(b"#"):
            continue
        if len(raw) >= MAX_LINE_BYTES or b"\0" in raw:
            raise ValueError("manifest field line is too long or contains a NUL")
        try:
            key, value = raw.decode("ascii").split("=", 1)
        except (UnicodeDecodeError, ValueError) as exc:
            raise ValueError("malformed manifest field line") from exc
        if key not in allowed or not value:
            raise ValueError(f"unknown or empty manifest field: {key}")
        if key in entries:
            raise ValueError(f"duplicate manifest field: {key}")
        entries[key] = value
    if entries.get("format") != "1":
        raise ValueError("manifest format is missing or unsupported")
    version = validate_version(entries.get("version"))
    if "min_from" in entries:
        validate_version(entries["min_from"])
    present = 0
    for arch in ARCHES:
        keys = [f"{arch}_{field}" for field in ("file", "size", "sha256")]
        count = sum(key in entries for key in keys)
        if not count:
            continue
        if count != 3:
            raise ValueError(f"incomplete manifest asset: {arch}")
        if entries[keys[0]] != f"greencurve-{version}-windows-{arch}-setup.exe":
            raise ValueError(f"manifest filename does not match its version/architecture: {arch}")
        size = entries[keys[1]]
        if not re.fullmatch(r"[1-9][0-9]{0,8}", size) or int(size) > MAX_ASSET_BYTES:
            raise ValueError(f"manifest asset size is out of range: {arch}")
        if not re.fullmatch(r"[0-9a-f]{64}", entries[keys[2]]):
            raise ValueError(f"manifest asset digest must be lowercase SHA-256: {arch}")
        present += 1
    if not present:
        raise ValueError("manifest describes no assets")
    return entries
