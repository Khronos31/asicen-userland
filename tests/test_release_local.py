# SPDX-License-Identifier: GPL-2.0-only
"""ASICEN's temporary local release binding; no network or hardware required."""

import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "Khronos31/asicen-userland"
SHA = "a" * 40
OTHER_SHA = "b" * 40


class LocalWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.wrapper = (ROOT / ".github/workflows/release_tag.yml").read_text()
        self.shared = (ROOT / ".github/workflows/release_reusable.yml").read_text()
        self.tests = (ROOT / ".github/workflows/release_tests.yml").read_text()
        self.verify, self.publish = self.shared.split("  publish:\n", 1)

    def bootstrap(self, repository=REPOSITORY, revision=SHA, sha=SHA):
        source = self.verify.split("          python3 - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            env = {"VERIFIER_REF": revision, "GITHUB_REPOSITORY": repository,
                   "GITHUB_SHA": sha, "GITHUB_OUTPUT": str(output)}
            # No token, run ID or external referenced_workflows response exists.
            # Any unexpected bootstrap network access must fail the test.
            with patch.dict(os.environ, env, clear=True), \
                    patch("urllib.request.build_opener", side_effect=AssertionError("unexpected network")), \
                    patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
                try:
                    exec(compile(textwrap.dedent(source), "asicen-workflow-bootstrap", "exec"), {})
                except SystemExit:
                    self.assertFalse(output.exists(), "failed bootstrap must not expose a trusted SHA")
                    raise
            self.assertEqual(output.read_text(), f"sha={revision}\n")

    def allowed(self, job, *, repository=REPOSITORY, product="asicen-userland",
                event="push", ref="refs/tags/v0.1.0", deleted=False, result="success"):
        block = self.verify if job == "verify" else self.publish
        condition = block.split("    if: >-\n", 1)[1].split("    runs-on:", 1)[0]
        expression = " ".join(condition.split()).replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"\bfalse\b", "False", expression)
        context = {
            "github": SimpleNamespace(repository=repository, event_name=event, ref=ref,
                                      event=SimpleNamespace(deleted=deleted)),
            "inputs": SimpleNamespace(product=product),
            "needs": SimpleNamespace(verify=SimpleNamespace(result=result)),
            "startsWith": lambda value, prefix: value.startswith(prefix),
        }
        return eval(expression, {"__builtins__": {}}, context)

    def test_local_bootstrap_needs_no_px4_reference_or_network(self):
        self.bootstrap()
        self.assertNotIn("referenced_workflows", self.shared)
        self.assertNotIn("urllib", self.shared)

    def test_local_bootstrap_rejects_different_sha(self):
        with self.assertRaisesRegex(SystemExit, "revision differ"):
            self.bootstrap(revision=OTHER_SHA)

    def test_local_bootstrap_rejects_floating_malformed_or_nonexact_sha(self):
        for revision in ("main", "v0.1.0", SHA[:7], SHA.upper(), SHA + "\n", "g" * 40, ""):
            with self.subTest(revision=revision), self.assertRaises(SystemExit):
                self.bootstrap(revision=revision, sha=revision)

    def test_local_bootstrap_rejects_px4_forks_and_other_repositories(self):
        for repository in ("Khronos31/px4-userland", "attacker/asicen-userland", "", REPOSITORY + "-fork"):
            with self.subTest(repository=repository), self.assertRaisesRegex(SystemExit, "Unapproved"):
                self.bootstrap(repository=repository)

    def test_actual_workflow_accepts_only_asicen_non_deleted_version_push(self):
        rejected = [
            {"repository": "Khronos31/px4-userland", "product": "px4-userland"},
            {"repository": "attacker/asicen-userland"},
            {"product": "px4-userland"}, {"product": "unknown"}, {"deleted": True},
            {"ref": "refs/heads/main"}, {"ref": "refs/heads/v0.1.0"}, {"ref": "refs/tags/0.1.0"},
        ] + [{"event": event} for event in
             ("pull_request", "pull_request_target", "workflow_dispatch", "workflow_run")]
        for job in ("verify", "publish"):
            self.assertTrue(self.allowed(job))
            for change in rejected:
                with self.subTest(job=job, change=change):
                    self.assertFalse(self.allowed(job, **change))
        for result in ("failure", "cancelled", "skipped", ""):
            with self.subTest(result=result):
                self.assertFalse(self.allowed("publish", result=result))

    def test_local_wrapper_helper_checkouts_and_output_bind_same_sha(self):
        self.assertIn("uses: ./.github/workflows/release_reusable.yml", self.wrapper)
        self.assertIn("verifier-ref: ${{ github.sha }}", self.wrapper)
        self.assertIn("product: asicen-userland", self.wrapper)
        self.assertIn("github.repository == 'Khronos31/asicen-userland'", self.wrapper)
        self.assertIn("verifier-ref: ${{ steps.verifier.outputs.sha }}", self.verify)
        self.assertIn("ref: ${{ inputs.verifier-ref }}", self.verify)
        self.assertIn("VERIFIER_REF: ${{ inputs.verifier-ref }}", self.verify)
        self.assertIn("ref: ${{ needs.verify.outputs.verifier-ref }}", self.publish)
        self.assertIn("VERIFIER_REF: ${{ needs.verify.outputs.verifier-ref }}", self.publish)
        for block in (self.verify, self.publish):
            self.assertEqual(block.count("repository: " + REPOSITORY), 1)
            self.assertIn('test "$(git -C release-tools rev-parse HEAD)" = "$VERIFIER_REF"', block)
            self.assertIn("persist-credentials: false", block)
        self.assertIn("ref: ${{ github.sha }}", self.tests)
        self.assertIn("persist-credentials: false", self.tests)

    def test_no_external_px4_fetch_or_reusable_workflow(self):
        active = self.wrapper + self.shared + self.tests
        self.assertNotIn("Khronos31/px4-userland", active)
        self.assertNotIn("git clone", active)
        self.assertNotIn("git fetch", active)
        for usage in re.findall(r"^\s*uses:\s*(\S+)", active, re.MULTILINE):
            if usage == "./.github/workflows/release_reusable.yml":
                continue
            self.assertRegex(usage, r"^actions/(checkout|upload-artifact)@[0-9a-f]{40}$")
        self.assertEqual(re.findall(r"^\s*repository:\s*(\S+)", active, re.MULTILINE),
                         [REPOSITORY, REPOSITORY])

    def test_publish_shell_bootstrap_rechecks_exact_tag_sha(self):
        source = self.publish.split("        run: |\n", 1)[1].split("      - name:", 1)[0]
        source = textwrap.dedent(source)
        for revision, sha, accepted in ((SHA, SHA, True), (OTHER_SHA, SHA, False),
                                        ("main", "main", False), (SHA + "\n", SHA + "\n", False)):
            with self.subTest(revision=revision, sha=sha):
                result = subprocess.run(["bash", "-e", "-c", source], check=False,
                                        env={"PATH": os.defpath, "VERIFIER_REF": revision, "GITHUB_SHA": sha},
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)

    def test_real_workflows_preserve_readonly_verification_and_publish_gate(self):
        self.assertIn("cancel-in-progress: false", self.wrapper)
        self.assertIn("permissions: {}", self.shared)
        self.assertIn("contents: read", self.verify)
        self.assertNotIn("contents: write", self.verify)
        self.assertIn("contents: write", self.publish)
        self.assertIn("needs: verify", self.publish)
        self.assertIn("release_publish.py verify", self.verify)
        self.assertIn("release_publish.py publish", self.publish)
        for text in (self.wrapper, self.shared):
            self.assertNotIn("environment:", text)
            self.assertNotIn("secrets: inherit", text)
            self.assertNotIn("workflow_dispatch:", text)
            self.assertNotIn("workflow_run:", text)

    def test_offline_suite_runs_local_tests_and_tracks_test_changes(self):
        self.assertEqual(self.tests.count("      - tests/**"), 2)
        self.assertEqual(self.tests.count("      - third_party/release-automation/**"), 2)
        self.assertEqual(self.tests.count("      - THIRD_PARTY_NOTICES.md"), 2)
        self.assertIn("python3 -m unittest discover -s tests -p 'test_release_*.py' -v", self.tests)
        self.assertNotIn("contents: write", self.tests)

    def test_upstream_exact_copies_match_documented_frozen_hashes(self):
        provenance = (ROOT / "THIRD_PARTY_NOTICES.md").read_text()
        self.assertIn("b4635226082161ce09f811b1571c9dd25f40cf03", provenance)
        self.assertIn("dd1c1bea16a5871085d5f2a42c009364c1ec7345", provenance)
        exact = re.findall(r"^\| `([^`]+)` \| `[^`]+` \| `([0-9a-f]{64})` \| exact \|$",
                           provenance, re.MULTILINE)
        self.assertEqual({path for path, _ in exact}, {
            "tests/fixtures/release-upstream/release_tag.yml",
            "tests/fixtures/release-upstream/release_reusable.yml",
            "third_party/release-automation/LICENSE",
        })
        for path in ("scripts/release_verify.py", "tests/test_release_verify.py",
                     "tests/test_release_integration.py"):
            self.assertRegex(provenance, rf"(?m)^\| `{re.escape(path)}` \| `[^`]+` \| `[0-9a-f]{{64}}` \| local ASICEN firmware checks \|$")
        for path, expected in exact:
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)

    def test_real_http_boundary_regressions_also_cover_asicen_profile(self):
        import test_release_publish as boundary
        # Exercise the real GitHub URL/request/download implementation with
        # mocked openers, not the orchestration FakeAPI. No network is used.
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(boundary.HTTPTests)
        result = unittest.TestResult()
        with patch.object(boundary, "REPOSITORY", REPOSITORY), \
                patch.object(boundary, "PREFIX", "/repos/" + REPOSITORY):
            suite.run(result)
        self.assertGreater(result.testsRun, 0)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(result.skipped, [])

    def test_pending_record_example_is_not_a_completed_release_record(self):
        import test_release_publish as boundary
        with self.assertRaisesRegex(boundary.pub.release_verify.VerificationError, "release record"):
            boundary.pub.release_verify.validate_record(
                {"schema": 1, "status": "pending"}, product="asicen-userland",
                tag="v0.1.0", repository=REPOSITORY)

    def test_pending_notes_cannot_reach_release_writes(self):
        import test_release_publish as boundary
        # Use the upstream REST fixture; this test exercises the actual shared
        # heading/body gate before any artifact or release operation.
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(boundary, "REPOSITORY", REPOSITORY), \
                patch.object(boundary, "PREFIX", "/repos/" + REPOSITORY):
            api = boundary.FakeAPI()
            api.repository, api.prefix = REPOSITORY, "/repos/" + REPOSITORY
            api.commits[boundary.SHA] = {"sha": boundary.SHA, "tree": {"sha": boundary.TREE}}
            api.documents = {"VERSION": b"1.2.3\n", "release-notes/v1.2.3.md":
                             b"PENDING: Tests and the user's release-note request are not complete.\n"}
            publisher = boundary.pub.Publisher(api, "asicen-userland", boundary.TAG,
                                               boundary.SHA, Path(directory))
            with self.assertRaisesRegex(boundary.pub.ReleaseError, "heading or body"):
                publisher.prepare()
            self.assertEqual(api.mutations, [])
            self.assertEqual(api.downloads, [])


if __name__ == "__main__":
    unittest.main()
