# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Deterministic release-publication tests; no real keys or network access."""

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import release_post
import update_signing

_REPO = "example/green-curve"
_VERSION = "1.2.3"
_COMMIT = "a" * 40
_OTHER_COMMIT = "b" * 40
_TAG = "c" * 40


class ReleasePublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="greencurve-release-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "source").mkdir()
        (self.root / "source" / "update_verify_keys.h").write_text(
            "unsigned char GC_UPDATE_PUBLIC_KEY_ACTIVE[64] = {" +
            ", ".join(["0x01"] * 64) + "};", encoding="utf-8")
        (self.root / "CHANGELOG.md").write_text(
            f"## {_VERSION}\nRelease fixture.\n", encoding="utf-8")
        self.key = self.root / "placeholder.txt"
        self.key.write_text("placeholder; signing is mocked", encoding="utf-8")
        self.events = []
        self.commands = []
        self.output = io.StringIO()
        self.remote = {}
        self.downloads = {}
        for arch in ("x64", "arm64"):
            name = update_signing.asset_name(_VERSION, arch)
            payload = ("installer fixture " + arch).encode()
            self.downloads[name] = payload
            self.downloads[name + ".sha256"] = (
                hashlib.sha256(payload).hexdigest() + "  " + name + "\n").encode()
        self.assets = list(self.downloads)
        self.remote_object = {"type": "commit", "sha": _COMMIT}
        self.tag_objects = {}
        self.local_commit = _COMMIT
        self.bad_provenance = None
        self.source_commit = _COMMIT
        self.signer_workflow = f"{_REPO}/.github/workflows/release.yml"
        self.signature_failure = False
        self.network_corrupt = None
        for name, replacement in (("run_command", self.command), ("fetch_url", self.fetch)):
            patcher = patch.object(release_post, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)

    def command(self, cmd, cwd=None):
        self.commands.append(cmd)
        if cmd[0] == "git":
            if self.local_commit is None:
                raise RuntimeError("fixture local tag missing")
            return self.local_commit
        if cmd[:2] == ["gh", "api"]:
            endpoint = cmd[2]
            if "/git/ref/tags/" in endpoint:
                return json.dumps({"object": self.remote_object})
            if "/git/tags/" in endpoint:
                return json.dumps({"object": self.tag_objects[endpoint.rsplit("/", 1)[1]]})
            raise AssertionError(cmd)
        if cmd[:3] == ["gh", "release", "view"]:
            if cmd[-1] == "body":
                return json.dumps({"body": "Release fixture."})
            return json.dumps({"isDraft": False, "isPrerelease": False,
                               "assets": [{"name": name} for name in self.assets]})
        if cmd[:3] == ["gh", "release", "download"]:
            self.events.append("download")
            directory = Path(cmd[cmd.index("--dir") + 1])
            for name, payload in self.downloads.items():
                (directory / name).write_bytes(payload)
            self.remote.update(self.downloads)
            return ""
        if cmd[:3] == ["gh", "attestation", "verify"]:
            arch = "arm64" if "arm64" in cmd[3] else "x64"
            self.events.append("attest-" + arch)
            if self.bad_provenance == arch:
                raise RuntimeError("fixture provenance rejected: " + arch)
            for flag, actual in (("--source-digest", self.source_commit),
                                 ("--signer-workflow", self.signer_workflow)):
                # Model gh: absent constraints do not reject other commits/workflows.
                if flag in cmd and cmd[cmd.index(flag) + 1] != actual:
                    raise RuntimeError("fixture provenance constraint rejected: " + flag)
            return ""
        if cmd[0] != "gh" and cmd[2] == "prepare":
            self.events.append("sign")
            directory = Path(cmd[cmd.index("--dir") + 1])
            (directory / "greencurve-update-manifest.txt").write_text(
                update_signing.build_manifest(_VERSION, directory), encoding="utf-8", newline="\n")
            (directory / "greencurve-update-manifest.sig").write_bytes(b"mock signature\n")
            (directory / "greencurve-update-v2.txt").write_bytes(
                release_post.update_freshness.build_envelope((directory / "greencurve-update-manifest.txt").read_bytes()))
            (directory / "greencurve-update-v2.sig").write_bytes(b"mock signature\n")
            return ""
        if cmd[0] != "gh" and cmd[2] == "verify":
            if self.signature_failure:
                raise RuntimeError("fixture signature rejected")
            return ""
        if cmd[:3] == ["gh", "release", "upload"]:
            self.events.append("publish")
            for name in cmd[6:]:
                path = Path(name)
                self.remote[path.name] = path.read_bytes()
                self.assets.append(path.name)
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

    def run_release(self, **kwargs):
        with contextlib.redirect_stdout(self.output):
            return release_post.run_post_release(
                _REPO, _VERSION, self.key, self.root, **kwargs)

    def assert_not_signed_or_published(self):
        self.assertNotIn("sign", self.events)
        self.assertNotIn("publish", self.events)
        self.assertNotIn("greencurve-update-manifest.txt", self.remote)

    def test_both_architectures_verified_before_signing_and_publication(self):
        self.assertTrue(self.run_release())
        for arch in ("x64", "arm64"):
            self.assertLess(self.events.index("attest-" + arch), self.events.index("sign"))
        self.assertLess(self.events.index("sign"), self.events.index("publish"))
        commands = [cmd for cmd in self.commands if cmd[:3] == ["gh", "attestation", "verify"]]
        self.assertEqual(len(commands), 2)
        for cmd in commands:
            self.assertEqual(cmd[cmd.index("--source-digest") + 1], _COMMIT)
            self.assertEqual(cmd[cmd.index("--signer-workflow") + 1], self.signer_workflow)

    def test_invalid_provenance_never_signs_or_publishes(self):
        for arch in ("x64", "arm64"):
            with self.subTest(arch=arch):
                self.events.clear()
                self.bad_provenance = arch
                with self.assertRaisesRegex(RuntimeError, "provenance rejected"):
                    self.run_release()
                self.assert_not_signed_or_published()

    def test_other_commit_never_signs_or_publishes(self):
        self.source_commit = _OTHER_COMMIT
        with self.assertRaisesRegex(RuntimeError, "source-digest"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_other_workflow_never_signs_or_publishes(self):
        self.signer_workflow = f"{_REPO}/.github/workflows/other.yml"
        with self.assertRaisesRegex(RuntimeError, "signer-workflow"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_missing_asset_never_signs_or_publishes(self):
        self.assets.remove(update_signing.asset_name(_VERSION, "arm64"))
        with self.assertRaisesRegex((ValueError, FileNotFoundError), "missing"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_missing_checksum_asset_is_rejected_before_download(self):
        self.assets.remove(update_signing.asset_name(_VERSION, "x64") + ".sha256")
        with self.assertRaisesRegex(ValueError, "missing"):
            self.run_release()
        self.assertNotIn("download", self.events)
        self.assert_not_signed_or_published()

    def test_missing_download_never_signs_or_publishes(self):
        del self.downloads[update_signing.asset_name(_VERSION, "arm64")]
        with self.assertRaises(FileNotFoundError):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_checksum_mismatch_never_signs_or_publishes(self):
        self.downloads[update_signing.asset_name(_VERSION, "arm64")] += b"wrong"
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_empty_checksum_never_signs_or_publishes(self):
        self.downloads[update_signing.asset_name(_VERSION, "x64") + ".sha256"] = b""
        with self.assertRaisesRegex(ValueError, "invalid checksum"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_annotated_remote_tag_is_peeled(self):
        self.remote_object = {"type": "tag", "sha": _TAG}
        self.tag_objects[_TAG] = {"type": "commit", "sha": _COMMIT}
        self.assertTrue(self.run_release(dry_run=True))
        self.assertIn(["gh", "api", f"repos/{_REPO}/git/tags/{_TAG}"], self.commands)

    def test_remote_tag_must_match_reviewed_local_tag(self):
        self.remote_object["sha"] = _OTHER_COMMIT
        with self.assertRaisesRegex(ValueError, "commit.*mismatch|mismatch.*commit"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_cyclic_remote_tag_is_rejected(self):
        self.remote_object = {"type": "tag", "sha": _TAG}
        self.tag_objects[_TAG] = self.remote_object
        with self.assertRaisesRegex(ValueError, "tag"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_remote_tag_to_noncommit_is_rejected(self):
        self.remote_object["type"] = "tree"
        with self.assertRaisesRegex(ValueError, "tag"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_missing_local_tag_requires_explicit_reviewed_commit(self):
        self.local_commit = None
        with self.assertRaisesRegex(ValueError, "source-commit"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_explicit_full_commit_does_not_require_local_tag(self):
        self.local_commit = None
        self.assertTrue(self.run_release(dry_run=True, source_commit=_COMMIT))
        self.assertFalse(any(cmd[0] == "git" for cmd in self.commands))

    def test_abbreviated_commit_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "commit"):
            self.run_release(source_commit=_COMMIT[:7])
        self.assert_not_signed_or_published()

    def test_nonimmutable_local_tag_result_is_rejected(self):
        self.local_commit = "main"
        with self.assertRaisesRegex(ValueError, "commit"):
            self.run_release()
        self.assert_not_signed_or_published()

    def test_dry_run_verifies_both_architectures_without_publication(self):
        self.assertTrue(self.run_release(dry_run=True))
        for arch in ("x64", "arm64"):
            self.assertLess(self.events.index("attest-" + arch), self.events.index("sign"))
        self.assertNotIn("publish", self.events)
        self.assertNotIn("anonymous-fetch", self.events)

    def test_dry_run_rejects_bad_provenance_without_signing(self):
        self.bad_provenance = "arm64"
        with self.assertRaises(RuntimeError):
            self.run_release(dry_run=True)
        self.assert_not_signed_or_published()

    def test_local_signature_failure_never_publishes(self):
        self.signature_failure = True
        with self.assertRaisesRegex(RuntimeError, "signature rejected"):
            self.run_release()
        self.assertNotIn("publish", self.events)

    def test_anonymous_manifest_corruption_is_rejected(self):
        self.network_corrupt = "greencurve-update-manifest.txt"
        with self.assertRaisesRegex(ValueError, "network manifest"):
            self.run_release()
        self.assertNotIn("SUCCESS:", self.output.getvalue())

    def test_anonymous_freshness_corruption_is_rejected(self):
        self.network_corrupt = "greencurve-update-v2.txt"
        with self.assertRaisesRegex(ValueError, "published freshness"):
            self.run_release()
        self.assertNotIn("SUCCESS:", self.output.getvalue())

    def test_anonymous_installer_corruption_is_rejected(self):
        self.network_corrupt = update_signing.asset_name(_VERSION, "arm64")
        with self.assertRaisesRegex(ValueError, "size mismatch"):
            self.run_release()
        self.assertNotIn("SUCCESS:", self.output.getvalue())


def run_tests():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReleasePublicationTests)
    result = unittest.TextTestRunner(stream=sys.stderr).run(suite)
    if not result.wasSuccessful():
        raise RuntimeError("release_post regression tests failed")


if __name__ == "__main__":
    run_tests()
