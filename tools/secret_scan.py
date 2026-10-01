# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Pinned, redacted full-history secret scanning for Linux CI and releases."""
import hashlib
from pathlib import Path
import subprocess
import tarfile
import urllib.request

VERSION = "8.30.1"
ARCHIVE_SHA256 = "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb"


def main():
    root = Path(__file__).resolve().parent.parent
    work = root / "build-tmp" / "secret-scan"
    work.mkdir(parents=True, exist_ok=True)
    archive = work / "gitleaks.tar.gz"
    url = (f"https://github.com/gitleaks/gitleaks/releases/download/v{VERSION}/"
           f"gitleaks_{VERSION}_linux_x64.tar.gz")
    archive.write_bytes(urllib.request.urlopen(url, timeout=60).read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit("secret scanner archive digest mismatch")
    # Extract only the named regular executable, never arbitrary archive paths.
    with tarfile.open(archive) as bundle:
        member = bundle.getmember("gitleaks")
        if not member.isfile():
            raise SystemExit("secret scanner is not a regular executable")
        executable = work / "gitleaks"
        executable.write_bytes(bundle.extractfile(member).read())
    executable.chmod(0o700)
    subprocess.run([str(executable), "git", str(root), "--log-opts=--all",
                    "--redact", "--config", str(root / ".gitleaks.toml")], check=True)


if __name__ == "__main__":
    main()
