#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Unit and regression tests for tools/simulate_release.py."""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import simulate_release


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


def run_tests():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SimulateReleaseTests)
    runner = unittest.TextTestRunner(stream=sys.stdout, verbosity=2)
    result = runner.run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
