# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Offline executable-level profile transfer checks; requires a built client."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def verify(executable: Path, workspace: Path) -> None:
    seed = (workspace / "tests/fuzz-corpus/profile_codec/precise.gcprofile").read_text(encoding="utf-8")
    scratch = workspace / "build-tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="profiles-\u66f2\u7ebf-", dir=scratch) as temporary:
        folder = Path(temporary)
        config = folder / "config.ini"
        incoming = folder / "incoming.gcprofile"
        exported = folder / "exported.gcprofile"
        original = b"[profiles]\nlogon_slot=3\n[hotkeys]\nslot3=123\n[preferences]\nkeep=unchanged\n"
        config.write_bytes(original)
        incoming.write_text(seed, encoding="utf-8", newline="\n")

        def run(arguments, expected=0, answer=""):
            result = subprocess.run([str(executable), *map(str, arguments)], input=answer.encode(),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25,
                                    cwd=folder, check=False)
            text = result.stdout.decode("utf-8", errors="replace")
            assert result.returncode == expected, (arguments, result.returncode, text,
                                                    result.stderr.decode(errors="replace"))
            return text

        import_args = ["--import-profiles", incoming, "--slots", "3", "--config", config]
        preview = run(import_args, expected=3)
        assert "logon, hotkey" in preview and config.read_bytes() == original
        run([*import_args, "--confirm-import"], expected=3, answer="CANCEL\n")
        assert config.read_bytes() == original
        run([*import_args, "--confirm-import"], answer="IMPORT\n")
        saved = config.read_bytes()
        assert saved.startswith(original) and b"document.memory.offset_hz=-500000" in saved
        assert b"document.point.0.offset_hz=0" in saved
        run(["--export-profiles", exported, "--slots", "3", "--config", config])
        exact = exported.read_text(encoding="utf-8")
        assert "kind=driver_offset\noffset_hz=0" in exact
        assert "numerator=249\ndenominator=250" in exact
        assert "offset_hz=-500000" in exact
        # A fresh process reads stored intent; no selected GPU/service is needed.
        run(["--export-profiles", exported, "--config", config])
        assert exported.read_text(encoding="utf-8") == exact

        header, record = seed.split("[profile.1]", 1)
        bank = header.replace("profile_count=1", "profile_count=5")
        for ordinal in range(1, 6):
            bank += ("[profile.1]" + record).replace("profile.1", f"profile.{ordinal}").replace(
                "document_id=1", f"document_id={ordinal}") + "\n"
        incoming.write_text(bank, encoding="utf-8", newline="\n")
        run(["--import-profiles", incoming, "--slots", "1,2,3,4,5", "--confirm-import", "--config", config],
            answer="IMPORT\n")
        run(["--export-profiles", exported, "--slots", "all", "--config", config])
        bank_exact = exported.read_bytes()
        assert b"profile_count=5" in bank_exact and b"[profile.5.memory]" in bank_exact
        second = folder / "second.ini"
        run(["--import-profiles", exported, "--slots", "1,2,3,4,5", "--confirm-import", "--config", second],
            answer="IMPORT\n")
        run(["--export-profiles", incoming, "--config", second])
        assert incoming.read_bytes() == bank_exact
        saved = config.read_bytes()
        # Normalized aliases cannot turn an export into a config overwrite.
        run(["--export-profiles", folder / "." / "config.ini", "--config", config], expected=2)
        assert config.read_bytes() == saved
        incoming.write_text(seed + "\n[unknown]\nfield=1\n", encoding="utf-8", newline="\n")
        run(import_args, expected=2)
        assert config.read_bytes() == saved
        run(["--import-profiles", folder / "missing.gcprofile", "--slots", "1", "--config", config], expected=1)
        assert config.read_bytes() == saved
    print("Offline profile CLI checks passed: review/cancel, exact zero/fraction/ratio, five-slot restart/export, malformed input")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    options = parser.parse_args()
    verify(options.executable.resolve(), Path(__file__).resolve().parents[1])
