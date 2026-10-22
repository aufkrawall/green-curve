#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Unit and regression tests for tools/simulate_release.py."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, nullcontext
from pathlib import Path
from unittest.mock import patch

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import simulate_release
import simulate_release_guards as guards
import update_signing
import windows_config_lock
import windows_service_control as scm


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

    def steps(self):
        return [name for name, _ in self.calls if name != "query"]


class SimulateReleaseTests(unittest.TestCase):
    def test_parse_repo(self):
        owner, repo = simulate_release.parse_repo("aufkrawall/green-curve-update-test")
        self.assertEqual(owner, "aufkrawall")
        self.assertEqual(repo, "green-curve-update-test")

        owner, repo = simulate_release.parse_repo("user-name/repo.name_v2")
        self.assertEqual(owner, "user-name")
        self.assertEqual(repo, "repo.name_v2")

        with self.assertRaises(ValueError):
            simulate_release.parse_repo("invalid_repo_without_owner")
        with self.assertRaises(ValueError):
            simulate_release.parse_repo("owner/repo with spaces")
        with self.assertRaises(ValueError):
            simulate_release.parse_repo("owner/repo/extra")

    def test_sha256_file(self):
        with tempfile.NamedTemporaryFile(delete=False) as tf:
            tf.write(b"GreenCurveSimulationTestBytes")
            path = Path(tf.name)
        try:
            expected = hashlib.sha256(b"GreenCurveSimulationTestBytes").hexdigest().lower()
            self.assertEqual(simulate_release.sha256_file(path), expected)
        finally:
            path.unlink(missing_ok=True)

    def test_patch_update_repo(self):
        tmp = Path(tempfile.mkdtemp(prefix="gc-test-patch-"))
        try:
            src_dir = tmp / "source"
            src_dir.mkdir()
            header = src_dir / "update_url_policy.h"
            original_content = (
                '#ifndef GREEN_CURVE_UPDATE_URL_POLICY_H\n'
                '#define GREEN_CURVE_UPDATE_URL_POLICY_H\n\n'
                '#define GC_UPDATE_REPO_OWNER "aufkrawall"\n'
                '#define GC_UPDATE_REPO_NAME  "green-curve"\n\n'
                '#define GC_UPDATE_MANIFEST_ASSET "greencurve-update-manifest.txt"\n'
                '#endif\n'
            )
            header.write_text(original_content, encoding="utf-8")

            simulate_release.patch_update_repo(tmp, "test-owner", "test-repo")
            patched = header.read_text(encoding="utf-8")

            self.assertIn('#define GC_UPDATE_REPO_OWNER "test-owner"', patched)
            self.assertIn('#define GC_UPDATE_REPO_NAME  "test-repo"', patched)
            self.assertNotIn('"aufkrawall"', patched)
            self.assertNotIn('"green-curve"', patched)

            # Test failure on missing macro
            bad_header = src_dir / "update_url_policy.h"
            bad_header.write_text('#define SOMETHING_ELSE 1\n', encoding="utf-8")
            with self.assertRaises(RuntimeError):
                simulate_release.patch_update_repo(tmp, "test-owner", "test-repo")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_get_active_pubkey_hex(self):
        pubkey = simulate_release.get_active_pubkey_hex(ROOT_DIR)
        self.assertEqual(len(pubkey), 128)
        self.assertTrue(all(c in "0123456789abcdef" for c in pubkey))

    def test_get_latest_stable_tag(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            subprocess.run(["git", "init"], cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["git", "config", "user.name", "test"], cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["git", "config", "user.email", "test@test.local"], cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["git", "commit", "--allow-empty", "-m", "init"], cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for t in ["0.25.0", "0.27.0", "0.26.1", "invalid-tag"]:
                subprocess.run(["git", "tag", t], cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            highest = simulate_release.get_latest_stable_tag(tmp_path)
            self.assertEqual(highest, "0.27.0")

        try:
            tag = simulate_release.get_latest_stable_tag(ROOT_DIR)
            self.assertTrue(tag.startswith("0."))
            # Must parse as major.minor[.patch]
            parts = [int(p) for p in tag.split(".")]
            self.assertGreaterEqual(len(parts), 2)
        except RuntimeError as exc:
            if "could not find any valid stable release tags" in str(exc):
                return
            raise

    def test_cli_subcommands_parse(self):
        # Verify parser handles all documented subcommands without crashing
        with self.assertRaises(SystemExit) as cm:
            old_argv = sys.argv
            try:
                sys.argv = ["simulate_release.py", "--help"]
                simulate_release.main()
            finally:
                sys.argv = old_argv
        self.assertEqual(cm.exception.code, 0)


def _key_array(name, raw):
    rows = "\n".join("    " + ", ".join(f"0x{b:02X}" for b in raw[i:i + 8]) + ","
                     for i in range(0, 64, 8))
    return f"static const unsigned char {name}[GC_UPDATE_PUBLIC_KEY_BYTES] = {{\n{rows}\n}};\n"


def _fake_root(tmp, active_raw, next_raw, owner="aufkrawall", name="green-curve"):
    src = tmp / "source"
    src.mkdir(parents=True, exist_ok=True)
    (src / "update_verify_keys.h").write_text(
        "#define GC_UPDATE_PUBLIC_KEY_BYTES 64\n// keep this comment\n" +
        _key_array("GC_UPDATE_PUBLIC_KEY_ACTIVE", active_raw) + "\n" +
        _key_array("GC_UPDATE_PUBLIC_KEY_NEXT", next_raw) + "#define TRAILER 1\n",
        encoding="utf-8")
    (src / "update_url_policy.h").write_text(
        f'#define GC_UPDATE_REPO_OWNER "{owner}"\n#define GC_UPDATE_REPO_NAME  "{name}"\n',
        encoding="utf-8")
    return tmp


class SimulationGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gc-sim-guard-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _make_key(self, name):
        path = self.tmp / name
        update_signing.write_private_key(str(path), update_signing.generate_private_key())
        pub = guards.simulation_pubkey_hex(path, _fake_root(self.tmp / "other",
                                                            b"\x01" * 64, b"\x02" * 64))
        return path, pub

    def test_patch_update_keys_replaces_both_slots_only(self):
        root = _fake_root(self.tmp / "wt", b"\xAA" * 64, b"\xBB" * 64)
        sim = bytes(range(64)).hex()
        guards.patch_update_keys(root, sim)
        text = (root / "source" / "update_verify_keys.h").read_text(encoding="utf-8")
        self.assertEqual(guards._key_array_hex(text, "GC_UPDATE_PUBLIC_KEY_ACTIVE"), sim)
        self.assertEqual(guards._key_array_hex(text, "GC_UPDATE_PUBLIC_KEY_NEXT"), sim)
        self.assertNotIn("0xAA", text)
        self.assertNotIn("0xBB", text)
        self.assertIn("// keep this comment", text)
        self.assertIn("#define TRAILER 1", text)
        broken = self.tmp / "broken"
        (broken / "source").mkdir(parents=True)
        (broken / "source" / "update_verify_keys.h").write_text(
            _key_array("GC_UPDATE_PUBLIC_KEY_ACTIVE", b"\xAA" * 64), encoding="utf-8")
        with self.assertRaises(RuntimeError):
            guards.patch_update_keys(broken, sim)
        with self.assertRaises(ValueError):
            guards.patch_update_keys(root, "00" * 10)
        # The repository's real header (what every worktree build patches).
        real = self.tmp / "real"
        (real / "source").mkdir(parents=True)
        shutil.copy2(ROOT_DIR / "source" / "update_verify_keys.h", real / "source")
        guards.patch_update_keys(real, sim)
        patched = (real / "source" / "update_verify_keys.h").read_text(encoding="utf-8")
        self.assertEqual(guards._key_array_hex(patched, "GC_UPDATE_PUBLIC_KEY_ACTIVE"), sim)
        self.assertEqual(guards._key_array_hex(patched, "GC_UPDATE_PUBLIC_KEY_NEXT"), sim)
        self.assertIn("GC_UPDATE_UNINSTALL_KEY", patched)

    def test_release_keys_are_refused(self):
        key, pub = self._make_key("sim.txt")
        raw = bytes.fromhex(pub)
        # Trusted as ACTIVE or as NEXT by a release build: refused either way.
        for active, nxt in ((raw, b"\x01" * 64), (b"\x01" * 64, raw)):
            root = _fake_root(self.tmp / f"r{active[0]}", active, nxt)
            with self.assertRaises(RuntimeError):
                guards.simulation_pubkey_hex(key, root)
        # The release key's own path is refused before it is even read.
        with patch.object(guards, "get_release_key_path", return_value=key), \
             self.assertRaises(RuntimeError):
            guards.simulation_pubkey_hex(key, _fake_root(self.tmp / "o2", b"\x01" * 64,
                                                         b"\x02" * 64))
        # The repository's real keys are never the default simulation key.
        self.assertNotIn(pub, guards.get_release_pubkeys(ROOT_DIR))
        self.assertNotEqual(guards.get_default_key_path().name, "update-signing-key.txt")

    def test_ensure_simulation_key_creates_once(self):
        root = _fake_root(self.tmp / "o3", b"\x01" * 64, b"\x02" * 64)
        path = self.tmp / "keys" / "sim.txt"
        self.assertIsNone(guards.ensure_simulation_key(path, root, dry_run=True))
        self.assertFalse(path.exists())
        first = guards.ensure_simulation_key(path, root)
        self.assertEqual(guards.ensure_simulation_key(path, root), first)

    def test_production_repository_is_refused(self):
        root = _fake_root(self.tmp / "o4", b"\x01" * 64, b"\x02" * 64, "Owner", "Prod")
        for repo in ("Owner/Prod", "owner/prod", " OWNER/PROD "):
            with self.assertRaises(RuntimeError):
                guards.require_simulation_repo(repo, root)
        self.assertEqual(guards.require_simulation_repo("owner/prod-test", root), "owner/prod-test")
        with self.assertRaises(ValueError):
            guards.require_simulation_repo("not a repo", root)
        with self.assertRaises(RuntimeError):
            guards.require_simulation_repo(guards.get_production_repo(ROOT_DIR))
        guards.require_simulation_repo(simulate_release.DEFAULT_TEST_REPO)

    def test_clear_and_teardown_refuse_production_before_any_call(self):
        production = guards.get_production_repo(ROOT_DIR)
        calls = []
        with patch.object(simulate_release, "run_command",
                          side_effect=lambda *a, **k: calls.append(a) or "[]"):
            init_args = argparse.Namespace(repo=production, key=None, clear=True,
                                           dry_run=False, state=None)
            with self.assertRaises(RuntimeError):
                simulate_release.cmd_init(init_args)
            tear_args = argparse.Namespace(repo=production, versions=None, all_sim=True,
                                           private=True, state=None, seen_version=None,
                                           restore_seen_only=False)
            with self.assertRaises(RuntimeError):
                simulate_release.cmd_teardown(tear_args)
        self.assertEqual(calls, [])


INI = ("[meta]\r\nformat_version=2\r\n\r\n[updates]\r\nauto_check=2\r\n"
       "highest_seen_version=0.28.1\r\n\r\n[profiles]\r\nselected_slot=2\r\n")


class HighestSeenRestoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gc-sim-seen-"))
        # Real Windows synchronization on a scratch name; Linux CI exercises
        # the byte editor without claiming to hold a Windows machine mutex.
        name = "Local\\GreenCurveConfigFixture-" + self.tmp.name
        lock = (lambda: windows_config_lock.config_storage_lock(name)) \
            if os.name == "nt" else nullcontext
        self.lock_patch = patch.object(guards, "config_storage_lock", lock)
        self.lock_patch.start()
        self.addCleanup(self.lock_patch.stop)
        self.ini = self.tmp / "shared-profiles.ini"
        self.ini.write_bytes(INI.encode("latin-1"))
        self.state = self.tmp / "state.json"
        self.service = FakeService(self.ini)
        service_patch = patch.object(guards, "service_controller", lambda: self.service)
        service_patch.start()
        self.addCleanup(service_patch.stop)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_read_and_in_place_edit(self):
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28.1")
        identity = os.stat(self.ini).st_ino
        self.assertTrue(guards.set_highest_seen(self.ini, "0.28"))
        self.assertFalse(guards.set_highest_seen(self.ini, "0.28"))
        data = self.ini.read_bytes().decode("latin-1")
        self.assertEqual(data, INI.replace("0.28.1", "0.28"))
        # The same file object (its protected DACL and owner go with it).
        self.assertEqual(os.stat(self.ini).st_ino, identity)
        self.assertTrue(guards.set_highest_seen(self.ini, None))
        self.assertIsNone(guards.read_highest_seen(self.ini))
        self.assertIn("auto_check=2\r\n\r\n[profiles]", self.ini.read_bytes().decode("latin-1"))
        self.assertTrue(guards.set_highest_seen(self.ini, "0.27.0"))
        self.assertIn("auto_check=2\r\nhighest_seen_version=0.27.0\r\n\r\n[profiles]",
                      self.ini.read_bytes().decode("latin-1"))
        bare = self.tmp / "bare.ini"
        bare.write_bytes(b"[profiles]\nselected_slot=1\n")
        self.assertTrue(guards.set_highest_seen(bare, "0.27.0"))
        self.assertEqual(guards.read_highest_seen(bare), "0.27.0")
        self.assertIsNone(guards.read_highest_seen(self.tmp / "missing.ini"))

    def test_undecodable_value_round_trips(self):
        # A damaged or hand-edited value used to raise UnicodeDecodeError and
        # abort init/teardown before anything was recorded or restored.
        self.ini.write_bytes(INI.replace("0.28.1", "0.28.\xe91").encode("latin-1"))
        guards.record_pre_simulation_state(self.state, self.ini)
        recorded = json.loads(self.state.read_text(encoding="utf-8"))
        guards.set_highest_seen(self.ini, "9.9.9")
        self.assertTrue(guards.set_highest_seen(self.ini, recorded["highest_seen_version"]))
        self.assertIn(b"highest_seen_version=0.28.\xe91\r\n", self.ini.read_bytes())

    def test_record_is_never_overwritten(self):
        guards.record_pre_simulation_state(self.state, self.ini)
        guards.set_highest_seen(self.ini, "9.9.9")
        guards.record_pre_simulation_state(self.state, self.ini)
        recorded = json.loads(self.state.read_text(encoding="utf-8"))
        self.assertEqual(recorded["highest_seen_version"], "0.28.1")

    def test_restore_preserves_unrelated_bytes_and_line_endings(self):
        for ending in (b"\r\n", b"\n", b"\r"):
            for suffix in (b"", ending):
                original = ending.join((
                    b"[updates]", b"highest_seen_version=9.9", b"",
                    b"[profile1]", b"name=Quiet\x85", b"pattern=\xc3\x85 game")) + suffix
                with self.subTest(ending=ending, suffix=suffix):
                    self.ini.write_bytes(original)
                    self.assertTrue(guards.set_highest_seen(self.ini, "0.28"))
                    self.assertEqual(self.ini.read_bytes(),
                                     original.replace(b"9.9", b"0.28"))
                    self.assertTrue(guards.set_highest_seen(self.ini, None))
                    self.assertEqual(self.ini.read_bytes(), original.replace(
                        b"highest_seen_version=9.9" + ending, b""))

    def test_restore_reads_and_writes_inside_the_shared_lock(self):
        held = False
        entered = False
        read_bytes = Path.read_bytes
        original_open = open

        @contextmanager
        def config_lock():
            nonlocal held, entered
            # A writer finished while this caller was waiting for the lock.
            self.ini.write_bytes(INI.replace("selected_slot=2", "selected_slot=3")
                                 .encode("latin-1"))
            held = entered = True
            try:
                yield
            finally:
                held = False

        def read_locked(path):
            self.assertTrue(held, "configuration read preceded mutex acquisition")
            return read_bytes(path)

        def open_locked(*args, **kwargs):
            self.assertTrue(held, "configuration write outlived the mutex")
            return original_open(*args, **kwargs)

        with patch.object(guards, "config_storage_lock", config_lock, create=True), \
             patch.object(Path, "read_bytes", read_locked), \
             patch("builtins.open", side_effect=open_locked):
            self.assertTrue(guards.set_highest_seen(self.ini, "0.28"))
        self.assertTrue(entered)
        self.assertFalse(held)
        self.assertIn(b"selected_slot=3", self.ini.read_bytes())

    def test_restore_preserves_mixed_endings_and_unterminated_key(self):
        for original in (b"[profile1]\r\nname=Quiet\x85\n[updates]\rhighest_seen_version=9.9",
                         b"[updates]\nhighest_seen_version=9.9\r\n[profile1]\rname=Quiet\x85"):
            self.ini.write_bytes(original)
            self.assertEqual(guards.read_highest_seen(self.ini), "9.9")
            self.assertTrue(guards.set_highest_seen(self.ini, "0.28"))
            self.assertEqual(self.ini.read_bytes(), original.replace(b"9.9", b"0.28"))

    def test_restore_absent_key_removes_simulation_scaffolding_exactly(self):
        originals = (
            b"",
            b"[updates]",
            b"[meta]\r\n[updates]",
            b"[profiles]\nselected_slot=1",
            b"[profiles]\r\nselected_slot=1\r\n",
        )
        for original in originals:
            with self.subTest(original=original):
                self.ini.write_bytes(original)
                self.state.unlink(missing_ok=True)
                recorded = guards.record_pre_simulation_state(self.state, self.ini)
                layout = recorded["highest_seen_layout"]
                self.assertFalse(layout["highest_seen_present"])
                self.assertTrue(guards.set_highest_seen(self.ini, "9.9.9"))
                restored, _ = guards.restore_highest_seen(
                    self.state, self.ini, elevated=True)
                self.assertTrue(restored)
                self.assertIsNone(guards.read_highest_seen(self.ini))
                self.assertEqual(self.ini.read_bytes(), original)

    def test_cleanup_after_key_already_absent_requires_elevation_only_when_needed(self):
        original = b"[profiles]\nselected_slot=1"
        self.ini.write_bytes(original)
        guards.record_pre_simulation_state(self.state, self.ini)
        self.assertTrue(guards.set_highest_seen(self.ini, "9.9.9"))
        # Simulate an interrupted earlier restore that removed the key but left
        # the empty section and repair newline behind.
        self.ini.write_bytes(self.ini.read_bytes().replace(
            b"highest_seen_version=9.9.9\n", b""))
        restored, message = guards.restore_highest_seen(
            self.state, self.ini, elevated=False)
        self.assertFalse(restored)
        self.assertIn("--restore-seen-only", message)
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertEqual(self.ini.read_bytes(), original)

    def test_restore_absent_file_removes_simulation_only_config(self):
        self.ini.unlink(missing_ok=True)
        recorded = guards.record_pre_simulation_state(self.state, self.ini)
        self.assertFalse(recorded["highest_seen_layout"]["file_present"])
        # The installed client creates the file just to persist the seen mark.
        self.ini.write_bytes(b"[updates]\nhighest_seen_version=9.9.9\n")
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertFalse(self.ini.exists())

    def test_restore_absent_key_keeps_unrelated_update_settings(self):
        original = b"[profiles]\nselected_slot=1"
        self.ini.write_bytes(original)
        guards.record_pre_simulation_state(self.state, self.ini)
        self.assertTrue(guards.set_highest_seen(self.ini, "9.9.9"))
        self.ini.write_bytes(self.ini.read_bytes().replace(
            b"highest_seen_version=9.9.9\n",
            b"highest_seen_version=9.9.9\nauto_check=2\n"))
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        data = self.ini.read_bytes()
        self.assertNotIn(b"highest_seen_version", data)
        self.assertIn(b"auto_check=2", data)
        self.assertIn(b"[updates]", data)

    def test_mutex_failure_prevents_read_and_write(self):
        with patch.object(guards, "config_storage_lock", side_effect=TimeoutError("busy")), \
             patch.object(Path, "read_bytes", side_effect=AssertionError("read without lock")):
            with self.assertRaises(TimeoutError):
                guards.read_highest_seen(self.ini)
            with self.assertRaises(TimeoutError):
                guards.set_highest_seen(self.ini, "0.28")
        self.assertEqual(self.ini.read_bytes(), INI.encode("latin-1"))

    def test_restore_needs_elevation_and_prints_the_command(self):
        self.state.write_text(json.dumps({"highest_seen_version": "0.28"}), encoding="utf-8")
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=False)
        self.assertFalse(restored)
        self.assertIn("--restore-seen-only", message)
        self.assertIn(str(self.state.resolve()), message)
        self.assertIn("simulate_release.py", message)
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28.1")
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")
        self.assertIn("was not running", message)
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=False)
        self.assertTrue(restored)  # already equal: nothing to do, no elevation needed

    def test_restore_absent_record_and_override(self):
        restored, message = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertFalse(restored)
        self.assertIn("--seen-version", message)
        restored, _ = guards.restore_highest_seen(self.state, self.ini, override="",
                                                  elevated=True)
        self.assertTrue(restored)
        self.assertIsNone(guards.read_highest_seen(self.ini))
        # A record of "absent" removes the raised key again.
        guards.set_highest_seen(self.ini, "0.28.1")
        self.state.write_text(json.dumps({"highest_seen_version": None}), encoding="utf-8")
        restored, _ = guards.restore_highest_seen(self.state, self.ini, elevated=True)
        self.assertTrue(restored)
        self.assertIsNone(guards.read_highest_seen(self.ini))

    def test_restore_only_teardown_makes_no_github_call(self):
        self.state.write_text(json.dumps({"highest_seen_version": "0.28"}), encoding="utf-8")
        args = argparse.Namespace(repo=simulate_release.DEFAULT_TEST_REPO, versions=None,
                                  all_sim=False, private=True, state=str(self.state),
                                  seen_version=None, restore_seen_only=True,
                                  no_elevate=False, report=None)
        with patch.object(simulate_release, "run_command",
                          side_effect=AssertionError("no GitHub call expected")), \
             patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=True):
            self.assertEqual(simulate_release.cmd_teardown(args), 0)
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")
        self.assertFalse(self.state.exists())
        # Unelevated with elevation declined: the record stays and the exit
        # code says it is unfinished.
        self.state.write_text(json.dumps({"highest_seen_version": "0.27.0"}), encoding="utf-8")
        declined = []

        def decline(state, override):
            declined.append(state)
            return False, "elevation was declined"

        with patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=False), \
             patch.object(simulate_release, "run_elevated_restore", decline):
            self.assertEqual(simulate_release.cmd_teardown(args), 2)
        self.assertEqual(declined, [self.state])
        self.assertTrue(self.state.exists())
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")
        # --no-elevate never asks; an elevated child reports through --report.
        report = self.tmp / "report.json"
        args.no_elevate, args.report = True, str(report)
        with patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=False), \
             patch.object(simulate_release, "run_elevated_restore",
                          side_effect=AssertionError("no elevation expected")):
            self.assertEqual(simulate_release.cmd_teardown(args), 2)
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["restored"], False)
        with patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=True):
            self.assertEqual(simulate_release.cmd_teardown(args), 0)
        result = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(result["restored"])
        self.assertIn("0.28 -> 0.27.0", result["message"])
        self.assertFalse(self.state.exists())

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
        self.assertEqual([c for c in self.service.calls if c[0] != "query"],
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

    def test_unelevated_restore_hands_over_to_the_elevation_hook(self):
        self._record()
        asked = []

        def elevate(state, override):
            asked.append((state, override))
            return True, "child restored"

        restored, message = guards.restore_highest_seen(
            self.state, self.ini, override="0.27.0", elevated=False, elevate=elevate)
        self.assertEqual((restored, message), (True, "child restored"))
        self.assertEqual(asked, [(self.state, "0.27.0")])
        self.assertEqual(self.service.calls, [])
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28.1")

    def test_elevated_child_round_trip(self):
        launched = []

        def launch(executable, args, cwd):
            launched.append(args)
            report = Path(args[args.index("--report") + 1])
            report.write_text(json.dumps({"restored": True, "message": "restored x"}),
                              encoding="utf-8")
            return 0

        restored, message = guards.run_elevated_restore(self.state, "", launch=launch)
        self.assertTrue(restored)
        self.assertEqual(message, "elevated: restored x")
        args = launched[0]
        self.assertEqual(args[1:4], ["teardown", "--restore-seen-only", "--no-elevate"])
        self.assertEqual(args[args.index("--state") + 1], str(self.state.resolve()))
        self.assertEqual(args[args.index("--seen-version") + 1], "")
        self.assertFalse((self.tmp / guards.RESTORE_REPORT_NAME).exists())
        # The empty override survives the command line as an empty argument.
        self.assertIn('--seen-version ""', subprocess.list2cmdline(args))

    def test_elevated_child_failures_print_the_command(self):
        def declined(executable, args, cwd):
            raise OSError(1223, "elevation was declined")

        def silent(executable, args, cwd):
            return 1

        def refused(executable, args, cwd):
            Path(args[args.index("--report") + 1]).write_text(
                json.dumps({"restored": False, "message": "stalled"}), encoding="utf-8")
            return 2

        for launch, expected in ((declined, "declined"), (silent, "left no report"),
                                 (refused, "stalled")):
            with self.subTest(expected=expected):
                restored, message = guards.run_elevated_restore(self.state, launch=launch)
                self.assertFalse(restored)
                self.assertIn(expected, message)
                self.assertIn("--restore-seen-only", message)


class ServiceControlTests(unittest.TestCase):
    """tools/windows_service_control.py: the product's SCM wait protocol."""

    def test_constants_match_the_product(self):
        header = (ROOT_DIR / "source" / "service_scm_wait_policy.h").read_text(encoding="utf-8")
        defines = dict(re.findall(r"#define (GC_SCM_\w+) (\d+)u?l{0,2}", header))
        self.assertEqual(int(defines["GC_SCM_WAIT_NO_HINT_STALL_MS"]), scm.NO_HINT_STALL_MS)
        self.assertEqual(int(defines["GC_SCM_WAIT_MIN_STALL_MS"]), scm.MIN_STALL_MS)
        self.assertEqual(int(defines["GC_SCM_WAIT_MAX_TOTAL_MS"]), scm.MAX_TOTAL_MS)
        self.assertEqual(int(defines["GC_SCM_STATE_STOP_PENDING"]), scm.STATE_STOP_PENDING)
        self.assertEqual(int(defines["GC_SCM_STATE_RUNNING"]), scm.STATE_RUNNING)
        common = (ROOT_DIR / "source" / "installer_common.h").read_text(encoding="utf-8")
        self.assertIn(f'#define GC_SETUP_SERVICE_NAME L"{scm.SERVICE_NAME}"', common)

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

    @unittest.skipUnless(os.name == "nt", "Windows SCM")
    def test_native_query_is_read_only_and_names_absence(self):
        self.assertIsNone(scm.WindowsServiceController(
            f"GreenCurveAbsentFixture{os.getpid()}").query_state())
        # Always installed on Windows; querying needs no elevation.
        self.assertIn(scm.WindowsServiceController("EventLog").query_state(),
                      scm.STATE_NAMES)


class WindowsConfigLockTests(unittest.TestCase):
    def test_lock_contract_matches_the_application(self):
        source = (ROOT_DIR / "source" / "config_utils.cpp").read_text(encoding="utf-8")
        self.assertIn('"' + windows_config_lock.CONFIG_MUTEX_NAME.replace("\\", "\\\\") + '"',
                      source)
        # The C++ descriptor is a sequence of adjacent string literals.
        # SDDL itself contains semicolons: bound by its closing declaration.
        descriptor = source.split("CONFIG_INTERPROCESS_MUTEX_SDDL =", 1)[1].split('";', 1)[0]
        self.assertEqual("".join(re.findall(r'"([^\"]*)"', descriptor + '"')),
                         windows_config_lock.CONFIG_MUTEX_SDDL)
        self.assertEqual(windows_config_lock.CONFIG_MUTEX_ACCESS, 0x00100001)

    @unittest.skipUnless(os.name == "nt", "Windows named mutex fixture")
    def test_native_mutex_excludes_other_threads_and_releases(self):
        name = f"Local\\GreenCurveConfigLockFixture-{os.getpid()}"
        def acquire():
            try:
                with windows_config_lock.config_storage_lock(name, timeout_ms=0):
                    return "acquired"
            except TimeoutError:
                return "busy"
        with ThreadPoolExecutor(max_workers=1) as worker:
            with windows_config_lock.config_storage_lock(name):
                self.assertEqual(worker.submit(acquire).result(timeout=10), "busy")
            self.assertEqual(worker.submit(acquire).result(timeout=10), "acquired")
            with self.assertRaisesRegex(ValueError, "fixture"), \
                 windows_config_lock.config_storage_lock(name):
                raise ValueError("fixture")
            self.assertEqual(worker.submit(acquire).result(timeout=10), "acquired")


def run_tests():
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite()
    for case in (SimulateReleaseTests, SimulationGuardTests, HighestSeenRestoreTests,
                 ServiceControlTests, WindowsConfigLockTests):
        suite.addTests(loader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner(stream=sys.stderr).run(suite)
    # Raised, like the sibling release suites: the release self-test chain
    # (release_manifest -> release_post) calls this without reading a result.
    if not result.wasSuccessful():
        raise RuntimeError("simulate_release regression tests failed")
    return True


if __name__ == "__main__":
    run_tests()
