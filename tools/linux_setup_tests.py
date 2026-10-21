# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Desktop cleanup contract; native Linux also executes the shell helper."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile


def run_self_tests(root):
    source = (Path(root) / "tools" / "greencurve-setup.sh").read_text(encoding="utf-8")
    match = re.search(r"^remove_user_desktop_entry\(\) \{\n.*?^\}\n", source, re.M | re.S)
    if not match or 'remove_user_desktop_entry "$user" "$home"' not in source:
        raise RuntimeError("Linux desktop cleanup helper is not wired")
    helper = match.group()
    if 'runuser -u "$user" -- rm -f --' not in helper or re.search(r"^\s*rm\b", helper, re.M):
        raise RuntimeError("Linux desktop cleanup must use the account's privileges")
    if not sys.platform.startswith("linux"):
        print("Linux desktop cleanup: source checks passed; shell execution requires Linux")
        return
    bash = shutil.which("bash")
    if not bash:
        raise RuntimeError("Bash is required to verify the shipped Linux setup helper")
    with tempfile.TemporaryDirectory(prefix="gc-desktop-cleanup-") as temporary:
        directory = Path(temporary)
        home = directory / "home with spaces"
        for scenario in ("success", "failure", "unavailable"):
            captured = directory / "arguments"
            root_rm = directory / "root-rm"
            captured.unlink(missing_ok=True)
            root_rm.unlink(missing_ok=True)
            environment = dict(os.environ, PATH="", CAPTURE=str(captured), ROOT_RM=str(root_rm))
            environment.pop("BASH_ENV", None)
            mock = '' if scenario == "unavailable" else (
                'runuser() { printf "%s\\0" "$@" > "$CAPTURE"; return ' +
                ('1' if scenario == "failure" else '0') + '; }\n')
            harness = ('warn() { printf "%s\\n" "$*"; }\n'
                       'info() { printf "%s\\n" "$*"; }\n'
                       'rm() { printf root > "$ROOT_RM"; return 1; }\n' + mock +
                       helper + '\nremove_user_desktop_entry "$1" "$2"\n')
            result = subprocess.run([bash, "--noprofile", "--norc", "-c", harness,
                                     "fixture", "fixture-user", str(home)],
                                    env=environment, capture_output=True, text=True, timeout=10)
            if result.returncode or root_rm.exists():
                raise RuntimeError(f"Linux cleanup used a root fallback: {scenario}")
            if scenario != "unavailable":
                expected = ["-u", "fixture-user", "--", "rm", "-f", "--",
                            str(home / ".local/share/applications/greencurve.desktop")]
                if captured.read_bytes().split(b"\0")[:-1] != [x.encode() for x in expected]:
                    raise RuntimeError("Linux cleanup arguments were not preserved")
            elif captured.exists() or "unavailable" not in result.stdout:
                raise RuntimeError("Linux cleanup did not report an unavailable user switch")
    print("Linux desktop cleanup shell fixtures passed")
