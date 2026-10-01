# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Deterministic freshness-renewal tests; no real keys or network access."""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import release_post
import release_renew
import update_freshness
import update_signing

_REPO = "example/green-curve"
_VERSION = "1.2.3"
_COMMIT = "a" * 40
_NOW = 1_800_000_000


class FreshnessRenewalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="greencurve-renew-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "source").mkdir()
        (self.root / "source" / "update_verify_keys.h").write_text(
            "unsigned char GC_UPDATE_PUBLIC_KEY_ACTIVE[64] = {" +
            ", ".join(["0x01"] * 64) + "};", encoding="utf-8")
        self.key = self.root / "placeholder.txt"
        self.key.write_text("placeholder; signing is mocked", encoding="utf-8")
        staging = self.root / "staging"
        staging.mkdir()
        self.remote = {}
        for arch in ("x64", "arm64"):
            name = update_signing.asset_name(_VERSION, arch)
            self.remote[name] = ("installer fixture " + arch).encode()
            (staging / name).write_bytes(self.remote[name])
        self.v1 = update_signing.build_manifest(_VERSION, staging).encode("utf-8")
        self.remote[release_renew.V1_MANIFEST] = self.v1
        self.remote[release_renew.V1_SIGNATURE] = b"mock v1 signature\n"
        self.remote[update_freshness.MANIFEST_ASSET] = update_freshness.build_envelope(
            self.v1, _NOW - 40 * 24 * 3600)
        self.remote[update_freshness.SIGNATURE_ASSET] = b"old v2 signature\n"
        self.latest = _VERSION
        self.events, self.uploads = [], []
        self.bad_signature = None
        self.bad_provenance = None
        self.tamper_v1_during_renewal = False
        self.wrap_other_bytes = False
        self.network_corrupt = None
        self.output = io.StringIO()
        for name, replacement in (("run_command", self.command), ("fetch_url", self.fetch)):
            patcher = patch.object(release_post, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)

    def command(self, cmd, cwd=None):
        if cmd[0] == "git":
            return _COMMIT
        if cmd[:2] == ["gh", "api"]:
            if cmd[2].endswith("/releases/latest"):
                return json.dumps({"tag_name": self.latest})
            return json.dumps({"object": {"type": "commit", "sha": _COMMIT}})
        if cmd[:3] == ["gh", "release", "view"]:
            return json.dumps({"isDraft": False, "isPrerelease": False,
                               "assets": [{"name": name} for name in self.remote]})
        if cmd[:3] == ["gh", "release", "download"]:
            directory = Path(cmd[cmd.index("--dir") + 1])
            wanted = [cmd[i + 1] for i, arg in enumerate(cmd) if arg == "--pattern"]
            for name in wanted:
                (directory / name).write_bytes(self.remote[name])
            return ""
        if cmd[:3] == ["gh", "attestation", "verify"]:
            arch = "arm64" if "arm64" in cmd[3] else "x64"
            self.events.append("attest-" + arch)
            if self.bad_provenance == arch:
                raise RuntimeError("fixture provenance rejected")
            return ""
        if cmd[:3] == ["gh", "release", "upload"]:
            self.events.append("upload")
            self.uploads.append(cmd)
            for name in cmd[6:]:
                if name.startswith("--"):
                    continue
                self.remote[Path(name).name] = Path(name).read_bytes()
            return ""
        if cmd[2] == "verify":
            self.events.append("verify-" + Path(cmd[3]).name)
            if self.bad_signature and Path(cmd[3]).name == self.bad_signature:
                raise RuntimeError("fixture signature rejected")
            return ""
        if cmd[2] == "renew-freshness":
            self.events.append("sign")
            v1_path = Path(cmd[3])
            directory = Path(cmd[cmd.index("--dir") + 1])
            payload = v1_path.read_bytes()
            if self.tamper_v1_during_renewal:
                v1_path.write_bytes(payload + b"# tampered\n")
            if self.wrap_other_bytes:
                payload = payload.replace(b"version=", b"version=9")
            (directory / update_freshness.MANIFEST_ASSET).write_bytes(
                update_freshness.build_envelope(payload, _NOW))
            (directory / update_freshness.SIGNATURE_ASSET).write_bytes(b"new v2 signature\n")
            return ""
        raise AssertionError(cmd)

    def fetch(self, url, output_path=None):
        self.events.append("anonymous-fetch")
        name = url.rsplit("/", 1)[1]
        data = self.remote[name]
        if name == self.network_corrupt:
            data += b"corrupt"
        if output_path:
            Path(output_path).write_bytes(data)
        return data, "https://release-assets.githubusercontent.com/fixture/" + name

    def renew(self, **kwargs):
        with contextlib.redirect_stdout(self.output):
            return release_renew.run_renew_freshness(
                _REPO, _VERSION, self.key, self.root, now=_NOW, **kwargs)

    def assert_nothing_signed_or_uploaded(self):
        self.assertNotIn("sign", self.events)
        self.assertNotIn("upload", self.events)
        self.assertEqual(self.remote[release_renew.V1_MANIFEST], self.v1)

    def test_renews_only_the_v2_pair_over_unchanged_v1(self):
        self.assertTrue(self.renew())
        for arch in ("x64", "arm64"):
            self.assertLess(self.events.index("attest-" + arch), self.events.index("sign"))
        self.assertLess(self.events.index("verify-" + release_renew.V1_MANIFEST),
                        self.events.index("sign"))
        self.assertEqual(len(self.uploads), 1)
        uploaded = sorted(Path(arg).name for arg in self.uploads[0][6:] if not arg.startswith("--"))
        self.assertEqual(uploaded, sorted([update_freshness.MANIFEST_ASSET,
                                           update_freshness.SIGNATURE_ASSET]))
        self.assertIn("--clobber", self.uploads[0])
        self.assertEqual(self.remote[release_renew.V1_MANIFEST], self.v1)
        self.assertEqual(update_freshness.unwrap(
            self.remote[update_freshness.MANIFEST_ASSET], _NOW), self.v1)
        self.assertIn("SUCCESS:", self.output.getvalue())

    def test_non_latest_release_is_refused(self):
        self.latest = "9.9.9"
        with self.assertRaisesRegex(ValueError, "releases/latest"):
            self.renew()
        self.assert_nothing_signed_or_uploaded()

    def test_latest_is_rechecked_before_upload(self):
        original = self.command
        def publish_race(cmd, cwd=None):
            result = original(cmd, cwd)
            if cmd[0] != "gh" and cmd[2] == "renew-freshness":
                self.latest = "9.9.9"
            return result
        with patch.object(release_post, "run_command", publish_race):
            with self.assertRaisesRegex(ValueError, "releases/latest"):
                self.renew()
        self.assertIn("sign", self.events)
        self.assertNotIn("upload", self.events)

    def test_duplicate_manifest_key_is_refused_before_renewal(self):
        self.remote[release_renew.V1_MANIFEST] += b"min_from=1.0\nmin_from=1.1\n"
        with self.assertRaisesRegex(ValueError, "duplicate manifest field"):
            self.renew()
        self.assertNotIn("sign", self.events)
        self.assertNotIn("upload", self.events)

    def test_unverified_published_manifest_is_refused(self):
        self.bad_signature = release_renew.V1_MANIFEST
        with self.assertRaisesRegex(RuntimeError, "signature rejected"):
            self.renew()
        self.assert_nothing_signed_or_uploaded()

    def test_installer_not_named_by_manifest_is_refused(self):
        name = update_signing.asset_name(_VERSION, "arm64")
        self.remote[name] = self.remote[name] + b"swapped"
        with self.assertRaisesRegex(ValueError, "differs from the signed manifest"):
            self.renew()
        self.assert_nothing_signed_or_uploaded()

    def test_bad_provenance_is_refused_before_signing(self):
        self.bad_provenance = "x64"
        with self.assertRaisesRegex(RuntimeError, "provenance"):
            self.renew()
        self.assert_nothing_signed_or_uploaded()

    def test_renewal_that_alters_v1_is_never_uploaded(self):
        self.tamper_v1_during_renewal = True
        with self.assertRaisesRegex(ValueError, "modified the v1"):
            self.renew()
        self.assertNotIn("upload", self.events)

    def test_envelope_over_other_bytes_is_never_uploaded(self):
        self.wrap_other_bytes = True
        with self.assertRaisesRegex(ValueError, "exact published v1"):
            self.renew()
        self.assertNotIn("upload", self.events)

    def test_bad_new_signature_is_never_uploaded(self):
        self.bad_signature = update_freshness.MANIFEST_ASSET
        with self.assertRaisesRegex(RuntimeError, "signature rejected"):
            self.renew()
        self.assertNotIn("upload", self.events)

    def test_dry_run_uploads_nothing(self):
        self.assertTrue(self.renew(dry_run=True))
        self.assertIn("sign", self.events)
        self.assertNotIn("upload", self.events)
        self.assertNotIn("anonymous-fetch", self.events)

    def test_anonymous_corruption_is_reported(self):
        self.network_corrupt = update_freshness.MANIFEST_ASSET
        with self.assertRaisesRegex(ValueError, "published freshness bytes"):
            self.renew()
        self.assertNotIn("SUCCESS:", self.output.getvalue())


def run_tests():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FreshnessRenewalTests)
    result = unittest.TextTestRunner(stream=sys.stderr).run(suite)
    if not result.wasSuccessful():
        raise RuntimeError("release_renew regression tests failed")


if __name__ == "__main__":
    run_tests()
