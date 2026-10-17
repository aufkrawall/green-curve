# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Check pinned Arch recipe inputs without executing packaging shell code."""
import hashlib
from pathlib import Path
import re
import shlex


def check_recipe(path):
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    arrays = {name: shlex.split(body, comments=True) for name, body in re.findall(
        r"(?ms)^(source(?:_\w+)?|sha256sums(?:_\w+)?)=\((.*?)\)", text)}
    sources = [name for name in arrays if name == "source" or name.startswith("source_")]
    if not sources:
        raise ValueError(f"{path.name}: no source arrays")
    for name in sources:
        checksum_name = "sha256sums" + name[len("source"):]
        inputs = arrays[name]
        checksums = arrays.get(checksum_name, [])
        if len(inputs) != len(checksums):
            raise ValueError(f"{path.name}: {name}/{checksum_name} counts differ")
        for source, expected in zip(inputs, checksums):
            if not re.fullmatch(r"[0-9a-f]{64}", expected):
                raise ValueError(f"{path.name}: {source} needs a pinned SHA-256")
            # Remote inputs contain URL/shell substitutions. Their pinned
            # digests are checked here; makepkg verifies their delivered bytes.
            if "$" in source or "://" in source:
                continue
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", source) or source in (".", ".."):
                raise ValueError(f"{path.name}: unsupported local source: {source}")
            actual = hashlib.sha256((path.parent / source).read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f"{path.name}: {source} checksum differs from its current bytes")
    check_source_dir(path, text)


def check_source_dir(path, text):
    """A GitHub tag archive unpacks to `<repository>-<tag>/`; build() and
    package() must enter exactly that directory or makepkg fails."""
    if "/archive/refs/tags/$pkgver.tar.gz" not in text:
        return
    url = re.search(r'(?m)^url="([^"]+)"', text)
    if not url:
        raise ValueError(f"{path.name}: tag archive source without a url=")
    expected = url.group(1).rstrip("/").rsplit("/", 1)[1] + "-$pkgver"
    srcname = re.search(r'(?m)^_srcname="([^"]+)"', text)
    dirs = re.findall(r'(?m)^\s*cd "([^"]+)"', text)
    if not dirs:
        raise ValueError(f"{path.name}: tag archive source but no cd into it")
    for d in dirs:
        if d == "$_srcname" and srcname:
            d = srcname.group(1)
        if d != expected:
            raise ValueError(f"{path.name}: cd \"{d}\" but the tag archive unpacks to {expected}")


def check_all(directory):
    import arch_recipe_checks_tests
    arch_recipe_checks_tests.run_tests()
    for name in ("PKGBUILD", "PKGBUILD.bin"):
        check_recipe(Path(directory) / name)
    print("Arch recipe source pins and local asset checksums verified")
