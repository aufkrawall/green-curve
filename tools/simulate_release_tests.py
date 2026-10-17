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
        tag = simulate_release.get_latest_stable_tag(ROOT_DIR)
        self.assertTrue(tag.startswith("0."))
        # Must parse as major.minor[.patch]
        parts = [int(p) for p in tag.split(".")]
        self.assertGreaterEqual(len(parts), 2)

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
        with patch.object(guards, "get_release_key_path", return_value=key):
            with self.assertRaises(RuntimeError):
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
        self.assertIn("Restart", message)
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
                                  seen_version=None, restore_seen_only=True)
        with patch.object(simulate_release, "run_command",
                          side_effect=AssertionError("no GitHub call expected")), \
             patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=True):
            self.assertEqual(simulate_release.cmd_teardown(args), 0)
        self.assertEqual(guards.read_highest_seen(self.ini), "0.28")
        self.assertFalse(self.state.exists())
        # Unelevated: the record stays and the exit code says it is unfinished.
        self.state.write_text(json.dumps({"highest_seen_version": "0.27.0"}), encoding="utf-8")
        with patch.object(guards, "machine_config_path", return_value=self.ini), \
             patch.object(guards, "is_elevated", return_value=False):
            self.assertEqual(simulate_release.cmd_teardown(args), 2)
        self.assertTrue(self.state.exists())


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
            with self.assertRaisesRegex(ValueError, "fixture"):
                with windows_config_lock.config_storage_lock(name):
                    raise ValueError("fixture")
            self.assertEqual(worker.submit(acquire).result(timeout=10), "acquired")


def run_tests():
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite()
    for case in (SimulateReleaseTests, SimulationGuardTests, HighestSeenRestoreTests,
                 WindowsConfigLockTests):
        suite.addTests(loader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner(stream=sys.stderr).run(suite)
    # Raised, like the sibling release suites: the release self-test chain
    # (release_manifest -> release_post) calls this without reading a result.
    if not result.wasSuccessful():
        raise RuntimeError("simulate_release regression tests failed")
    return True


if __name__ == "__main__":
    run_tests()
