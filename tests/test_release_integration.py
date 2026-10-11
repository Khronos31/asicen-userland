# SPDX-License-Identifier: GPL-2.0-only
"""Full offline release pipeline: real verifier plus a fake GitHub boundary."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

# Import the archive fixture first so both fixtures share the verifier module.
import test_release_verify as archive_fixture
import test_release_publish as boundary

pub = boundary.pub
verify = pub.release_verify


def populate_api(api, fixture):
    """Expose complete synthetic bundles through the REST-shaped boundary."""
    api.tag_target = archive_fixture.TAG_SHA
    api.repository_info["full_name"] = fixture.repository
    api.documents = {
        "VERSION": (archive_fixture.VERSION + "\n").encode(),
        f"release-notes/{archive_fixture.TAG}.md": fixture.notes,
        f"release-notes/{archive_fixture.TAG}.json": fixture.record_path.read_bytes(),
    }
    for label, directory, run_id, artifact_id, sha, tree in (
            ("candidate", fixture.candidate, 100, 1000, archive_fixture.SHA, archive_fixture.TREE),
            ("reproduction", fixture.reproduction, 101, 1001, archive_fixture.SHA, archive_fixture.TREE),
            ("tag", fixture.tag_dir, 102, 1002, archive_fixture.TAG_SHA, archive_fixture.TAG_TREE)):
        repo = copy.deepcopy(api.repository_info)
        run = {"id": run_id, "run_attempt": 1, "workflow_id": 2, "path": pub.WORKFLOW,
               "repository": repo, "head_repository": repo, "head_sha": sha,
               "head_branch": archive_fixture.TAG if label == "tag" else "main", "event": "push",
               "pull_requests": [], "status": "completed", "conclusion": "success"}
        api.runs[run_id] = run
        payload = boundary.zip_bytes({path.name: path.read_bytes() for path in directory.iterdir()})
        artifact = {"id": artifact_id, "name": "release-candidate", "expired": False,
                    "expires_at": "2099-01-01T00:00:00Z", "digest": "sha256:" + verify.digest(payload),
                    "workflow_run": {"id": run_id, "head_sha": sha, "repository_id": 1,
                                     "head_repository_id": 1}}
        api.artifacts[artifact_id] = artifact
        api.run_artifacts[run_id] = [artifact]
        api.bytes[f"{api.prefix}/actions/artifacts/{artifact_id}/zip"] = payload
        api.commits[sha] = {"sha": sha, "tree": {"sha": tree}}
        # Use actual synthetic source bytes and Git blob object hashing for the
        # independent Git inventory check, not a second hardcoded acceptance map.
        source_dir = fixture.candidate if label == "reproduction" else directory
        source_name = f"{fixture.product}-{archive_fixture.VERSION}-source.tar.gz"
        files = fixture.files[(source_dir, source_name)]
        entries = []
        for name, (data, mode) in files.items():
            if not name.startswith("repository/"):
                continue
            blob = b"blob " + str(len(data)).encode() + b"\0" + data
            entries.append({"path": name[len("repository/"):], "type": "blob",
                            "mode": "100755" if mode & 0o111 else "100644",
                            "sha": hashlib.sha1(blob).hexdigest()})
        api.trees[tree] = {"sha": tree, "truncated": False, "tree": entries}
    api.listed_runs = [api.runs[102]]
    provenance = {"schema": 1, "repository": fixture.repository, "event": "push",
                  "ref": f"refs/tags/{archive_fixture.TAG}", "sha": archive_fixture.TAG_SHA,
                  "run_id": 102, "run_attempt": 1}
    payload = boundary.zip_bytes({"release-provenance.json": archive_fixture.encode(provenance)})
    artifact = copy.deepcopy(api.artifacts[1002])
    artifact.update(id=1003, name="release-provenance", digest="sha256:" + verify.digest(payload))
    api.artifacts[1003] = artifact
    api.run_artifacts[102].append(artifact)
    api.bytes[f"{api.prefix}/actions/artifacts/1003/zip"] = payload


class FullPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        firmware = patch.object(verify, "FIRMWARE_SHA256", verify.digest(archive_fixture.FIRMWARE))
        firmware.start()
        self.addCleanup(firmware.stop)
        self.assertIs(verify, archive_fixture.verify)

    def setup_product(self, product):
        repository = pub.REPOSITORIES[product]
        for name, value in (("REPOSITORY", repository), ("PREFIX", f"/repos/{repository}")):
            setting = patch.object(boundary, name, value)
            setting.start()
            self.addCleanup(setting.stop)
        fixture = archive_fixture.ReleaseFixture(self.root / "fixtures" / product, product, repository)
        api = boundary.FakeAPI()
        api.repository, api.prefix = repository, f"/repos/{repository}"
        populate_api(api, fixture)
        work = self.root / "work" / product
        work.mkdir(parents=True)
        publisher = pub.Publisher(api, product, archive_fixture.TAG, archive_fixture.TAG_SHA, work)
        return fixture, api, publisher

    def complete_product(self, product):
        fixture, api, publisher = self.setup_product(product)
        asset_count = len(verify.expected_assets(product, archive_fixture.VERSION))
        report = publisher.prepare()
        branch = pub.RELEASE_BRANCHES[product]
        self.assertEqual(report["release_branch"], branch)
        self.assertIn(("GET", f"{api.prefix}/git/ref/heads/{branch}"), api.calls)
        self.assertTrue(report["verification"]["verified"])
        self.assertEqual(report["tag_sha"], archive_fixture.TAG_SHA)
        self.assertEqual(report["verification"]["candidate_sha"], archive_fixture.SHA)
        self.assertEqual(api.mutations, [])
        self.assertEqual(report["verification"]["assets_sha256"], report["assets_sha256"])
        self.assertEqual(len(api.downloads), 4)  # Three builds plus exact tag-push provenance.
        result = publisher.publish()
        self.assertEqual(result["state"], "published-and-redownload-verified")
        self.assertEqual([call[0] for call in api.mutations],
                         ["POST"] + ["UPLOAD"] * asset_count + ["PATCH"])
        self.assertEqual(len([path for path in api.downloads if "/releases/assets/" in path]),
                         asset_count * 2)
        self.assertEqual(api.latest["body"].encode(), fixture.notes)
        self.assertEqual({asset["name"] for asset in api.assets}, set(publisher.names))
        self.assertEqual(product == "px4-userland", any("windows" in a["name"] for a in api.assets))
        # Retry repeats the complete verifier then observes the published release
        # read-only. It cannot repair or overwrite already-public assets.
        api.mutations.clear()
        again_dir = self.root / "retry" / product
        again_dir.mkdir(parents=True)
        again = pub.Publisher(api, product, archive_fixture.TAG, archive_fixture.TAG_SHA, again_dir)
        again.prepare()
        self.assertEqual(again.publish()["state"], "already-published-verified")
        self.assertEqual(api.mutations, [])

    def test_full_px4_candidate_tag_draft_latest_and_retry(self):
        self.complete_product("px4-userland")

    def test_full_asicen_candidate_tag_firmware_provenance_and_retry(self):
        self.complete_product("asicen-userland")

    def test_changed_payload_cannot_reach_any_release_write(self):
        fixture, api, publisher = self.setup_product("px4-userland")
        fixture.mutate("px4d", b"tampered, valid manifest and checksums")
        populate_api(api, fixture)  # New wrapper digest remains valid, inner payload is different.
        with self.assertRaisesRegex(verify.VerificationError, "payload changed"):
            publisher.prepare()
        self.assertEqual(api.mutations, [])

    def test_source_script_change_blocked_by_git_and_no_publication(self):
        fixture, api, publisher = self.setup_product("px4-userland")
        fixture.mutate("repository/scripts/build.sh", b"changed build command", source=True)
        populate_api(api, fixture)
        with self.assertRaisesRegex(pub.ReleaseError, "non-documentation Git change"):
            publisher.prepare()
        self.assertEqual(api.mutations, [])

    def test_asicen_does_not_substitute_default_main_for_reviewed_release_branch(self):
        _, api, publisher = self.setup_product("asicen-userland")
        api.branch_ref_override = "refs/heads/main"
        with self.assertRaisesRegex(pub.ReleaseError, "wrong release branch identity"):
            publisher.prepare()
        self.assertEqual(api.mutations, [])

    def test_asicen_tag_must_be_on_reviewed_release_branch(self):
        _, api, publisher = self.setup_product("asicen-userland")
        api.compare_status = "diverged"
        with self.assertRaisesRegex(pub.ReleaseError, "release/0.1.0"):
            publisher.prepare()
        self.assertEqual(api.mutations, [])


if __name__ == "__main__":
    unittest.main()
