#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""The release-simulation restore around a running service.

The service writes its in-memory seen version back on every settings save, so
the restore stops it; a clean stop returns the GPU to stock, so the applied
settings are saved first and re-applied after the start, as setup does.
Run through simulate_release_tests.run_tests().
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import nullcontext
from pathlib import Path
from unittest.mock import patch

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import simulate_release_guards as guards
import windows_config_lock
import windows_service_control as scm
import windows_settings_transfer as transfer

INI = ("[meta]\r\nformat_version=2\r\n\r\n[updates]\r\nauto_check=2\r\n"
       "highest_seen_version=0.28.1\r\n\r\n[profiles]\r\nselected_slot=2\r\n")


class FakeService:
    """A service that records each call with the seen value it found then."""

    name = "GreenCurveService"

    def __init__(self, ini, state=None, stop_error=None, start_error=None):
        self.ini = ini
        self.state = state
        self.stop_error = stop_error
        self.start_error = start_error
        self.calls = []

    def query_state(self):
        self.calls.append(("query", guards.read_highest_seen(self.ini)))
        return self.state

    def stop(self):
        self.calls.append(("stop", guards.read_highest_seen(self.ini)))
        if self.stop_error:
            raise self.stop_error
        self.state = scm.STATE_STOPPED

    def start(self):
        self.calls.append(("start", guards.read_highest_seen(self.ini)))
        if self.start_error:
            raise self.start_error
        self.state = scm.STATE_RUNNING

    def install_directory(self):
        return self.ini.parent

    def steps(self):
        return [name for name, _ in self.calls if name in ("stop", "start")]

    def sequence(self):
        return [name for name, _ in self.calls if name != "query"]


class FakeTransfer:
    """Settings export/apply recorded into the service's call list."""

    def __init__(self, captured=transfer.CAPTURE_SAVED, applied=True):
        self.captured = captured
        self.applied = applied
        self.service = None

    def bind(self, service):
        self.service = service
        return self

    def _note(self, name):
        self.service.calls.append((name, guards.read_highest_seen(self.service.ini)))

    def capture(self):
        self._note("capture")
        return self.captured, "fake export"

    def reapply(self):
        self._note("reapply")
        return self.applied, "fake apply exit 1"

    def discard(self):
        self._note("discard")


class ServiceRestartTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gc-sim-restart-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        name = "Local\\GreenCurveConfigFixture-" + self.tmp.name
        lock = (lambda: windows_config_lock.config_storage_lock(name)) \
            if os.name == "nt" else nullcontext
        for target, value in (("config_storage_lock", lock),
                              ("service_controller", lambda: self.service),
                              ("settings_transfer_for",
                               lambda service: self.transfer.bind(service))):
            patcher = patch.object(guards, target, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.ini = self.tmp / "shared-profiles.ini"
        self.ini.write_bytes(INI.encode("latin-1"))
        self.state = self.tmp / "state.json"
        self.service = FakeService(self.ini)
        self.transfer = FakeTransfer()

    # --- the restore holds only with the owning service stopped -------------

    def _record(self, value="0.28"):
        self.state.write_text(json.dumps({"highest_seen_version": value}), encoding="utf-8")

    def test_restore_stops_writes_verifies_and_restarts_a_running_service(self):
        # The defect: the running service wrote its in-memory simulated mark
        # back over a file-only restore at its next settings save.
        self._record()
        self.service.state = scm.STATE_RUNNING
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored, message)
        # Stopped while it still held the old value, started after the write.
        self.assertEqual([c for c in self.service.calls if c[0] in ("stop", "start")],
                         [("stop", "0.28.1"), ("start", "0.28")])
        self.assertIn("stopped GreenCurveService (was running)", message)
        self.assertIn("started GreenCurveService", message)

    def test_restore_leaves_a_stopped_or_absent_service_alone(self):
        for state in (None, scm.STATE_STOPPED):
            with self.subTest(state=state):
                self.ini.write_bytes(INI.encode("latin-1"))
                self._record()
                self.service = FakeService(self.ini, state)
                restored, message = guards.restore_highest_seen(self.state, self.ini,
                                                                elevated=True)
                self.assertTrue(restored, message)
                self.assertEqual(self.service.steps(), [])
                self.assertEqual(guards.read_highest_seen(self.ini), "0.28")

    def test_restore_waits_out_a_stopping_service_without_restarting_it(self):
        self._record()
        self.service.state = scm.STATE_STOP_PENDING
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertEqual(self.service.steps(), ["stop"])

    def test_restore_never_writes_under_a_service_that_did_not_stop(self):
        self._record()
        self.service = FakeService(self.ini, scm.STATE_RUNNING,
                                   stop_error=RuntimeError("did not reach stopped (stalled)"))
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("stalled", message)
        self.assertEqual(self.service.steps(), ["stop"])
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28.1")

    def test_failed_write_still_restarts_the_service(self):
        self._record()
        self.service.state = scm.STATE_RUNNING
        with patch.object(guards, "set_highest_seen", side_effect=OSError("disk full")):
            restored, message = guards.restore_highest_seen(self.state, self.ini,
                                                            elevated=True)
        self.assertFalse(restored)
        self.assertIn("disk full", message)
        self.assertEqual(self.service.steps(), ["stop", "start"])

    def test_write_that_does_not_read_back_is_a_failure(self):
        self._record()
        with patch.object(guards, "set_highest_seen", return_value=True):
            restored, message = guards.restore_highest_seen(self.state, self.ini,
                                                            elevated=True)
        self.assertFalse(restored)
        self.assertIn("reads back as 0.28.1", message)

    def test_service_that_does_not_start_again_is_reported(self):
        self._record()
        self.service = FakeService(self.ini, scm.STATE_RUNNING,
                                   start_error=OSError("access denied"))
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("was restored, but GreenCurveService did not start again", message)
        self.assertIn("sc start GreenCurveService", message)
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")

    # --- the applied settings survive the restart (setup does the same) -----

    def test_applied_settings_are_saved_before_the_stop_and_reapplied_after(self):
        # The defect: a clean stop returns the GPU to stock and a started
        # service applies nothing, so the bare restart dropped the profile.
        self._record()
        self.service.state = scm.STATE_RUNNING
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored, message)
        self.assertEqual(self.service.sequence(),
                         ["capture", "stop", "start", "reapply", "discard"])
        self.assertIn("saved the applied settings", message)
        self.assertIn("re-applied the saved settings", message)

    def test_settings_that_cannot_be_saved_leave_the_service_running(self):
        self._record()
        self.service.state = scm.STATE_RUNNING
        self.transfer.captured = transfer.CAPTURE_FAILED
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("was not stopped", message)
        self.assertEqual(self.service.sequence(), ["capture"])
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28.1")

    def test_nothing_applied_restarts_without_a_reapply(self):
        self._record()
        self.service.state = scm.STATE_RUNNING
        self.transfer.captured = transfer.CAPTURE_NONE_ACTIVE
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored, message)
        self.assertEqual(self.service.sequence(), ["capture", "stop", "start"])
        self.assertIn("no applied settings to carry over", message)

    def test_failed_reapply_is_reported_and_the_snapshot_discarded(self):
        self._record()
        self.service.state = scm.STATE_RUNNING
        self.transfer.applied = False
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("NOT re-applied (fake apply exit 1)", message)
        self.assertIn("apply your profile", message)
        self.assertEqual(self.service.sequence()[-2:], ["reapply", "discard"])
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")

    def test_failed_write_still_brings_the_settings_back(self):
        self._record()
        self.service.state = scm.STATE_RUNNING
        with patch.object(guards, "set_highest_seen", side_effect=OSError("disk full")):
            restored, message = guards.restore_highest_seen(self.state, self.ini,
                                                            elevated=True)
        self.assertFalse(restored)
        self.assertIn("disk full", message)
        self.assertEqual(self.service.sequence(),
                         ["capture", "stop", "start", "reapply", "discard"])

    def test_no_reapply_into_a_service_that_did_not_start(self):
        self._record()
        self.service = FakeService(self.ini, scm.STATE_RUNNING,
                                   start_error=OSError("access denied"))
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("NOT re-applied", message)
        self.assertEqual(self.service.sequence(), ["capture", "stop", "start", "discard"])

    def test_no_capture_for_a_service_already_stopping(self):
        self._record()
        self.service.state = scm.STATE_STOP_PENDING
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertEqual(self.service.sequence(), ["stop"])
