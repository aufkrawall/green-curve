# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Offline executable-level profile transfer checks; requires a built client."""
import argparse
from pathlib import Path
import subprocess
import sys
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
        # Invalid capture choices are rejected before contacting a GPU service.
        run(["--capture-current", exported, "--domains", "vf,vf"], expected=2)
        run(["--capture-current", config, "--config", config], expected=2)
        run(["--capture-current", exported, "--representation", "sampled"], expected=2)
        run(["--check-profile", incoming], expected=2)  # multi-profile file requires selection before any capture
        run(["--check-profile", incoming, "--profile", "0"], expected=2)
        run(["--check-profile", incoming, "--profile", "1", "--profile", "1"], expected=2)
        run(["--check-profile", incoming, "--apply-config"], expected=2)
        assert config.read_bytes() == saved
        incoming.write_text(seed + "\n[unknown]\nfield=1\n", encoding="utf-8", newline="\n")
        run(import_args, expected=2)
        run(["--check-profile", incoming], expected=2)
        assert config.read_bytes() == saved
        # Explicit source formats and choices preserve historical values without
        # invoking host-specific repair or consulting hardware.
        old = folder / "legacy.ini"
        old.write_bytes(("[profile2]\nmem_offset_mhz=-1001\n[profile2_curve]\npoint22=1000\n"
                         "[profile4]\nmem_offset_mhz=3\n").encode())
        legacy_args = ["--import-profiles", old, "--legacy-format", "linux-ini",
                       "--source-profiles", "4,2", "--slots", "1,5", "--config", config]
        run([*legacy_args, "--confirm-import"], expected=2, answer="IMPORT\n")
        assert config.read_bytes() == saved
        units = ["--legacy-memory-unit", "effective-mhz"]
        run([*legacy_args, *units, "--confirm-import"], expected=2, answer="IMPORT\n")
        assert config.read_bytes() == saved
        choices = [*units, "--legacy-curve-semantics", "absolute"]
        preview = run([*legacy_args, *choices], expected=3)
        assert "explicit mapping review" in preview and config.read_bytes() == saved
        run([*legacy_args, *choices, "--confirm-import"], answer="IMPORT\n")
        run(["--export-profiles", exported, "--slots", "5,1", "--config", config])
        exact_legacy = exported.read_text(encoding="utf-8")
        assert "offset_hz=-500500000" in exact_legacy and "offset_hz=1500000" in exact_legacy
        assert "legacy_indices_unresolved=1" in exact_legacy
        assert "target_hz=1000000000" in exact_legacy
        run(["--import-profiles", exported, "--source-profiles", "2", "--slots", "3",
             "--config", config, "--confirm-import"], answer="IMPORT\n")
        run(["--export-profiles", incoming, "--slots", "3", "--config", config])
        assert "offset_hz=1500000" in incoming.read_text(encoding="utf-8")
        # Old Unicode Windows files work as input and local offline export.
        old.write_bytes(("[profile1]\nmem_offset_mhz=-1001\n[profile1_curve]\n"
                         "curve_semantics=absolute_with_origin\npoint22_mhz=1000\n"
                         "point22_mv=650\npoint22_visible=0\n").encode("utf-16"))
        run(["--import-profiles", old, "--legacy-format", "windows-ini", "--slots", "2",
             "--config", config, "--confirm-import"], answer="IMPORT\n")
        run(["--export-profiles", incoming, "--slots", "1", "--config", old,
             "--legacy-format", "windows-ini"])
        windows_legacy = incoming.read_text(encoding="utf-8")
        assert "offset_hz=-1001000000" in windows_legacy and "voltage_uv=0" in windows_legacy
        assert old.read_bytes().startswith(b"\xff\xfe")
        if sys.platform == "win32":
            # Native source format is known for an ordinary local-bank export.
            run(["--export-profiles", incoming, "--slots", "1", "--config", old])
            assert incoming.read_text(encoding="utf-8") == windows_legacy
        elif sys.platform.startswith("linux"):
            native_old = folder / "native-legacy.ini"
            native_export = folder / "native-legacy.gcprofile"
            native_bytes = (b"[meta]\nlinux_mem_migrated=1\n[profile1]\nmem_offset_mhz=-1001\n"
                            b"[profile1_curve]\ncurve_semantics=absolute_with_origin\npoint22=1000\n")
            native_old.write_bytes(native_bytes)
            run(["--export-profiles", native_export, "--slots", "1", "--config", native_old])
            native_saved = native_export.read_text(encoding="utf-8")
            assert "offset_hz=-1001000000" in native_saved and "target_hz=1000000000" in native_saved
            assert "legacy_indices_unresolved=1" in native_saved and native_old.read_bytes() == native_bytes
        saved = config.read_bytes()
        unicode_bank = folder / "unicode.ini"
        unicode_text = "[profiles]\r\nlogon_slot=3\r\n[preferences]\r\nopaque=\u66f2\U0001f600\r\n"
        unicode_original = unicode_text.encode("utf-16")
        unicode_bank.write_bytes(unicode_original)
        unicode_import = ["--import-profiles", incoming, "--slots", "3", "--config", unicode_bank]
        preview = run(unicode_import, expected=3)
        assert "assignments=logon" in preview and unicode_bank.read_bytes() == unicode_original
        run([*unicode_import, "--confirm-import"], expected=3, answer="CANCEL\n")
        assert unicode_bank.read_bytes() == unicode_original
        run([*unicode_import, "--confirm-import"], answer="IMPORT\n")
        unicode_saved = unicode_bank.read_bytes()
        assert unicode_saved.startswith(unicode_original)
        assert "document.memory.offset_hz=-1001000000" in unicode_saved.decode("utf-16")
        run(["--export-profiles", exported, "--slots", "3", "--config", unicode_bank])
        assert exported.read_bytes() == incoming.read_bytes()
        # One malformed code unit refuses before review or replacement.
        unicode_bank.write_bytes(unicode_saved + b"\x00")
        run([*unicode_import, "--confirm-import"], expected=2, answer="IMPORT\n")
        assert unicode_bank.read_bytes() == unicode_saved + b"\x00"
        run(["--import-profiles", folder / "missing.gcprofile", "--slots", "1", "--config", config], expected=1)
        assert config.read_bytes() == saved
    print("Offline profile CLI checks passed: review/cancel, exact intent, five slots, legacy units/origins, source selection")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    options = parser.parse_args()
    verify(options.executable.resolve(), Path(__file__).resolve().parents[1])
