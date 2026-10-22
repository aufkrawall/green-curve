#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Tests for tools/windows_service_control.py and tools/windows_settings_transfer.py.

Run through simulate_release_tests.run_tests(), the release self-test chain.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import windows_service_control as scm
import windows_settings_transfer as transfer


def _source(name):
    return (ROOT_DIR / "source" / name).read_text(encoding="utf-8")


class ServiceControlTests(unittest.TestCase):
    """tools/windows_service_control.py: the product's SCM wait protocol."""

    def test_constants_match_the_product(self):
        header = _source("service_scm_wait_policy.h")
        defines = dict(re.findall(r"#define (GC_SCM_\w+) (\d+)u?l{0,2}", header))
        self.assertEqual(int(defines["GC_SCM_WAIT_NO_HINT_STALL_MS"]), scm.NO_HINT_STALL_MS)
        self.assertEqual(int(defines["GC_SCM_WAIT_MIN_STALL_MS"]), scm.MIN_STALL_MS)
        self.assertEqual(int(defines["GC_SCM_WAIT_MAX_TOTAL_MS"]), scm.MAX_TOTAL_MS)
        self.assertEqual(int(defines["GC_SCM_STATE_STOP_PENDING"]), scm.STATE_STOP_PENDING)
        self.assertEqual(int(defines["GC_SCM_STATE_RUNNING"]), scm.STATE_RUNNING)
        self.assertIn(f'#define GC_SETUP_SERVICE_NAME L"{scm.SERVICE_NAME}"',
                      _source("installer_common.h"))

    def test_wait_verdicts(self):
        stop, stopped = scm.STATE_STOP_PENDING, scm.STATE_STOPPED
        tracker = scm.WaitTracker(1000)
        self.assertEqual(tracker.step(1000, stop, 1, 3000, stopped), scm.WAIT_CONTINUE)
        # Quiet for exactly the hint is still alive; past it, stalled.
        self.assertEqual(tracker.step(4000, stop, 1, 3000, stopped), scm.WAIT_CONTINUE)
        self.assertEqual(tracker.step(4001, stop, 1, 3000, stopped), scm.WAIT_STALLED)
        # A checkpoint advance is progress.
        tracker = scm.WaitTracker(0)
        for now, check in ((0, 1), (2500, 2), (5000, 3)):
            self.assertEqual(tracker.step(now, stop, check, 3000, stopped), scm.WAIT_CONTINUE)
        self.assertEqual(tracker.step(5100, stopped, 0, 0, stopped), scm.WAIT_REACHED)
        # Settling somewhere else ends the wait at once.
        self.assertEqual(scm.WaitTracker(0).step(10, scm.STATE_RUNNING, 0, 0, stopped),
                         scm.WAIT_SETTLED_ELSEWHERE)
        # Progress never extends one wait past the overall bound.
        tracker = scm.WaitTracker(0)
        for now in range(0, scm.MAX_TOTAL_MS, 1000):
            self.assertEqual(tracker.step(now, stop, now, 3000, stopped), scm.WAIT_CONTINUE)
        self.assertEqual(tracker.step(scm.MAX_TOTAL_MS, stop, 99999, 3000, stopped),
                         scm.WAIT_OVERALL_TIMEOUT)

    def test_budgets_and_interval(self):
        self.assertEqual(scm.stall_budget_ms(0), scm.NO_HINT_STALL_MS)
        self.assertEqual(scm.stall_budget_ms(10), scm.MIN_STALL_MS)
        self.assertEqual(scm.stall_budget_ms(10 ** 9), scm.MAX_TOTAL_MS)
        self.assertEqual(scm.poll_interval_ms(0), 250)
        self.assertEqual(scm.poll_interval_ms(4000), 400)
        self.assertEqual(scm.poll_interval_ms(60000), 500)

    def test_image_path_executable(self):
        quoted = r'"C:\Program Files\Green Curve\greencurve-service.exe" --service-run'
        self.assertEqual(scm.image_path_executable(quoted),
                         Path(r"C:\Program Files\Green Curve\greencurve-service.exe"))
        self.assertEqual(scm.image_path_executable(r"C:\Apps\svc.exe -k x"),
                         Path(r"C:\Apps\svc.exe"))
        self.assertIsNone(scm.image_path_executable('"unterminated'))
        self.assertIsNone(scm.image_path_executable(""))
        self.assertIsNone(scm.image_path_executable(None))

    @unittest.skipUnless(os.name == "nt", "Windows SCM")
    def test_native_queries_are_read_only_and_name_absence(self):
        absent = scm.WindowsServiceController(f"GreenCurveAbsentFixture{os.getpid()}")
        self.assertIsNone(absent.query_state())
        self.assertIsNone(absent.install_directory())
        # Always installed on Windows; querying needs no elevation.
        event_log = scm.WindowsServiceController("EventLog")
        self.assertIn(event_log.query_state(), scm.STATE_NAMES)
        self.assertTrue(event_log.install_directory().is_dir())


class _Completed:
    def __init__(self, returncode, stdout=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = ""


class SettingsTransferTests(unittest.TestCase):
    """tools/windows_settings_transfer.py: setup's capture/re-apply contract."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gc-transfer-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.exe = self.tmp / transfer.GUI_EXE
        self.exe.write_bytes(b"MZ")
        self.calls = []

    def _scratch(self):
        folder = self.tmp / f"scratch{len(list(self.tmp.glob('scratch*')))}"
        folder.mkdir()
        return folder

    def _transfer(self, export_code, write=True, apply_code=0):
        def run(args, **kwargs):
            self.calls.append((args[1], args[2], kwargs.get("timeout")))
            if args[1] == "--export-active-settings":
                if write:
                    Path(args[2]).write_text("[profile1]\n", encoding="utf-8")
                return _Completed(export_code, "Active settings exported\n")
            return _Completed(apply_code, "ERROR: not ready\n" if apply_code else "")
        return transfer.SettingsTransfer(self.exe, run=run, make_scratch=self._scratch)

    def test_constants_match_setup(self):
        self.assertIn(f"GC_SETTINGS_TRANSFER_NO_ACTIVE_EXIT_CODE = {transfer.NO_ACTIVE_EXIT_CODE}",
                      _source("settings_transfer_exit_policy.h"))
        apply_cpp = _source("installer_apply.cpp")
        self.assertIn(f"#define GC_APP_EXPORT_TIMEOUT_MS {transfer.EXPORT_TIMEOUT_S * 1000}",
                      apply_cpp)
        self.assertIn(f"#define GC_APP_REAPPLY_TIMEOUT_MS {transfer.REAPPLY_TIMEOUT_S * 1000}",
                      apply_cpp)
        self.assertIn(f'L"{transfer.SCRATCH_SDDL}"', _source("installer_util.cpp"))
        self.assertIn(f'#define GC_SETUP_GUI_EXE "{transfer.GUI_EXE}"',
                      _source("installer_common.h"))

    def test_capture_result_matches_the_setup_taxonomy(self):
        result = transfer.capture_result
        self.assertEqual(result(True, 0, True), transfer.CAPTURE_SAVED)
        self.assertEqual(result(True, 4, False), transfer.CAPTURE_NONE_ACTIVE)
        for args in ((True, 0, False), (True, 4, True), (True, 1, False),
                     (False, None, True), (False, 0, True)):
            self.assertEqual(result(*args), transfer.CAPTURE_FAILED, args)

    def test_saved_settings_round_trip_and_are_discarded(self):
        carry = self._transfer(0)
        captured, detail = carry.capture()
        self.assertEqual(captured, transfer.CAPTURE_SAVED, detail)
        snapshot = carry.snapshot
        self.assertTrue(snapshot.is_file())
        applied, _ = carry.reapply()
        self.assertTrue(applied)
        self.assertEqual([c[:2] for c in self.calls],
                         [("--export-active-settings", str(snapshot)),
                          ("--apply-settings-file", str(snapshot))])
        self.assertEqual([c[2] for c in self.calls],
                         [transfer.EXPORT_TIMEOUT_S, transfer.REAPPLY_TIMEOUT_S])
        self.assertFalse(snapshot.exists())
        self.assertFalse(snapshot.parent.exists())

    def test_failed_apply_reports_the_cli_line(self):
        carry = self._transfer(0, apply_code=1)
        carry.capture()
        applied, detail = carry.reapply()
        self.assertFalse(applied)
        self.assertIn("exit 1: ERROR: not ready", detail)
        self.assertIsNone(carry.snapshot)

    def test_no_active_and_failures_leave_nothing_behind(self):
        for code, write, expected in ((4, False, transfer.CAPTURE_NONE_ACTIVE),
                                      (1, False, transfer.CAPTURE_FAILED),
                                      (0, False, transfer.CAPTURE_FAILED),
                                      (4, True, transfer.CAPTURE_FAILED)):
            with self.subTest(code=code, write=write):
                carry = self._transfer(code, write=write)
                captured, _ = carry.capture()
                self.assertEqual(captured, expected)
                self.assertIsNone(carry.snapshot)
                self.assertEqual(list(self.tmp.glob("scratch*/*")), [])
                self.assertFalse(carry.reapply()[0])

    def test_timeout_missing_binary_and_scratch_failure(self):
        def hang(args, **kwargs):
            raise subprocess.TimeoutExpired(args, kwargs.get("timeout"))
        carry = transfer.SettingsTransfer(self.exe, run=hang, make_scratch=self._scratch)
        captured, detail = carry.capture()
        self.assertEqual(captured, transfer.CAPTURE_FAILED)
        self.assertIn("timed out", detail)
        captured, detail = transfer.SettingsTransfer(self.tmp / "missing.exe").capture()
        self.assertEqual(captured, transfer.CAPTURE_FAILED)
        self.assertIn("no greencurve.exe", detail)
        self.assertEqual(transfer.SettingsTransfer(None).capture()[0], transfer.CAPTURE_FAILED)

        def no_scratch():
            raise OSError("access denied")
        captured, detail = transfer.SettingsTransfer(self.exe, make_scratch=no_scratch).capture()
        self.assertEqual(captured, transfer.CAPTURE_FAILED)
        self.assertIn("access denied", detail)

    @unittest.skipUnless(os.name == "nt", "Windows known folder")
    def test_native_program_files_is_the_known_folder(self):
        # Read-only: the scratch parent is the known folder setup uses, which a
        # standard user cannot write; creating the scratch itself needs admin.
        folder = transfer._program_files()
        self.assertTrue(folder.is_dir())
        self.assertEqual(os.path.normcase(folder),
                         os.path.normcase(os.environ.get("ProgramW6432") or
                                          os.environ["ProgramFiles"]))


if __name__ == "__main__":
    unittest.main()
