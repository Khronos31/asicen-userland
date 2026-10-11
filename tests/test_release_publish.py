# SPDX-License-Identifier: GPL-2.0-only
"""GitHub release orchestration tests. No network or repository mutation occurs."""

import base64
import copy
import hashlib
from contextlib import redirect_stderr
from io import BytesIO, StringIO
import json
from pathlib import Path
import stat
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import MagicMock, patch
from urllib.error import HTTPError
from urllib.parse import urlsplit
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import release_publish as pub


REPOSITORY = "Khronos31/px4-userland"
PREFIX = f"/repos/{REPOSITORY}"
SHA, SOURCE_SHA, TREE, SOURCE_TREE, TAG_OBJECT = (c * 40 for c in "abcde")
TAG = "v1.2.3"
NOTES = f"# px4-userland {TAG}\n\n変更を公開します。\n"


def zip_bytes(files):
    stream = BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        for name, content in files.items():
            archive.writestr(name, content)
    return stream.getvalue()


class FakeAPI:
    """A stateful in-memory REST boundary, with an audit log of every write."""

    repository = REPOSITORY
    prefix = PREFIX

    def __init__(self):
        self.calls = []
        self.mutations = []
        self.downloads = []
        self.releases = []
        self.assets = []
        self.latest = None
        self.runs = {}
        self.artifacts = {}
        self.run_artifacts = {}
        self.bytes = {}
        self.documents = {}
        self.commits = {}
        self.trees = {}
        self.tag_object = TAG_OBJECT
        self.tag_target = SHA
        self.tag_type = "tag"
        self.listed_runs = []
        self.repository_info = {"id": 1, "full_name": REPOSITORY, "fork": False, "default_branch": "main"}
        self.workflow = {"id": 2, "path": pub.WORKFLOW, "state": "active"}
        self.compare_status = "ahead"
        self.branch_ref_override = None
        self.next_asset = 1000

    def json(self, method, path, body=None, *, missing_ok=False):
        self.calls.append((method, path))
        route = urlsplit(path).path[len(PREFIX):]
        if method in {"POST", "PATCH", "DELETE"}:
            self.mutations.append((method, route, copy.deepcopy(body)))
        if route == "" and method == "GET":
            result = self.repository_info
        elif route.startswith("/git/ref/tags/"):
            result = {"ref": f"refs/tags/{TAG}", "object": {"type": self.tag_type, "sha": self.tag_object}}
        elif route.startswith("/git/ref/heads/"):
            branch = route[len("/git/ref/heads/"):]
            result = {"ref": self.branch_ref_override or f"refs/heads/{branch}",
                      "object": {"type": "commit", "sha": "f" * 40}}
        elif route.startswith("/git/tags/"):
            result = {"sha": self.tag_object, "tag": TAG, "object": {"type": "commit", "sha": self.tag_target}}
        elif route.startswith("/compare/"):
            result = {"status": self.compare_status}
        elif route.startswith("/git/commits/"):
            result = self.commits[route.rsplit("/", 1)[1]]
        elif route.startswith("/git/trees/"):
            result = self.trees[route.rsplit("/", 1)[1]]
        elif route.startswith("/contents/"):
            name = route[len("/contents/"):]
            raw = self.documents[name]
            result = {"type": "file", "path": name, "size": len(raw), "encoding": "base64",
                      "content": base64.b64encode(raw).decode()}
        elif route == "/actions/workflows/build_userland.yml":
            result = self.workflow
        elif route.startswith("/actions/runs/"):
            result = self.runs[int(route.split("/")[3])]
        elif route.startswith("/actions/artifacts/"):
            result = self.artifacts[int(route.split("/")[3])]
        elif route == "/releases/latest":
            result = self.latest
            if result is None and not missing_ok:
                raise pub.ReleaseError("no Latest release")
        elif route == "/releases" and method == "POST":
            result = {"id": 10, **body}
            self.releases.append(result)
        elif route.startswith("/releases/"):
            release_id = int(route.split("/")[2])
            result = next(item for item in self.releases if item["id"] == release_id)
            if method == "PATCH":
                result.update(body)
                self.latest = result
        else:
            raise AssertionError((method, path, body))
        return copy.deepcopy(result)

    def list(self, path, key=None):
        self.calls.append(("LIST", path))
        route = urlsplit(path).path[len(PREFIX):]
        if route == "/releases":
            result = self.releases
        elif route.endswith("/assets"):
            result = self.assets
        elif route.endswith("/artifacts"):
            result = self.run_artifacts[int(route.split("/")[3])]
        elif route.startswith("/actions/workflows/"):
            result = self.listed_runs
        else:
            raise AssertionError(path)
        return copy.deepcopy(result)

    def download(self, path, destination):
        self.downloads.append(path)
        Path(destination).write_bytes(self.bytes[path])

    def upload(self, release_id, file):
        self.mutations.append(("UPLOAD", release_id, file.name))
        require_names = [asset["name"] for asset in self.assets]
        if file.name in require_names:
            raise AssertionError("attempted duplicate upload")
        asset = {"id": self.next_asset, "name": file.name, "state": "uploaded", "size": file.stat().st_size}
        self.next_asset += 1
        self.assets.append(asset)
        self.bytes[f"{PREFIX}/releases/assets/{asset['id']}"] = file.read_bytes()
        return copy.deepcopy(asset)


def fixture(api):
    assets = {name: (name + "\n").encode() for name in pub.release_verify.expected_assets("px4-userland", "1.2.3")}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in assets.items()}
    record = {"notes_sha256": hashlib.sha256(NOTES.encode()).hexdigest()}
    api.documents = {"VERSION": b"1.2.3\n", f"release-notes/{TAG}.md": NOTES.encode()}
    repo = copy.deepcopy(api.repository_info)
    for run_id, sha, tree, label in [(11, SOURCE_SHA, SOURCE_TREE, "candidate"),
                                    (12, SOURCE_SHA, SOURCE_TREE, "reproduction"),
                                    (13, SHA, TREE, "tag")]:
        run = {"id": run_id, "run_attempt": 1, "workflow_id": 2, "path": pub.WORKFLOW,
               "repository": repo, "head_repository": repo, "head_sha": sha,
               "head_branch": TAG if label == "tag" else "main", "event": "push",
               "pull_requests": [], "status": "completed", "conclusion": "success"}
        api.runs[run_id] = run
        artifact_id = run_id + 100
        archive = zip_bytes(assets)
        artifact = {"id": artifact_id, "name": "release-candidate", "expired": False,
                    "expires_at": "2099-01-01T00:00:00Z", "digest": "sha256:" + hashlib.sha256(archive).hexdigest(),
                    "workflow_run": {"id": run_id, "head_sha": sha, "repository_id": 1, "head_repository_id": 1}}
        api.run_artifacts[run_id] = [artifact]
        api.artifacts[artifact_id] = artifact
        api.bytes[f"{PREFIX}/actions/artifacts/{artifact_id}/zip"] = archive
        api.commits[sha] = {"sha": sha, "tree": {"sha": tree}}
        api.trees[tree] = {"sha": tree, "truncated": False, "tree": [
            {"path": "README.md", "type": "blob", "mode": "100644", "sha": sha},
            {"path": "main.cpp", "type": "blob", "mode": "100644", "sha": "f" * 40},
        ]}
        if label != "tag":
            record[label] = {"run_id": run_id, "run_attempt": 1, "artifact_id": artifact_id,
                             "sha": sha, "tree": tree, "assets_sha256": hashes}
    api.listed_runs = [api.runs[13]]
    provenance = {"schema": 1, "repository": REPOSITORY, "event": "push", "ref": f"refs/tags/{TAG}",
                  "sha": SHA, "run_id": 13, "run_attempt": 1}
    archive = zip_bytes({"release-provenance.json": json.dumps(provenance).encode()})
    artifact = copy.deepcopy(api.artifacts[113])
    artifact.update({"id": 114, "name": "release-provenance", "digest": "sha256:" + hashlib.sha256(archive).hexdigest()})
    api.artifacts[114] = artifact
    api.run_artifacts[13].append(artifact)
    api.bytes[f"{PREFIX}/actions/artifacts/114/zip"] = archive
    api.documents[f"release-notes/{TAG}.json"] = json.dumps(record).encode()
    return record, assets


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.api = FakeAPI()
        self.record, self.assets = fixture(self.api)
        self.publisher = pub.Publisher(self.api, "px4-userland", TAG, SHA, self.temp.name)
        # Schema/archive enforcement has its own exhaustive tests. These mocks
        # isolate GitHub provenance and state transitions, not acceptance rules.
        self.validator = patch.object(pub.release_verify, "validate_record", side_effect=lambda record, **kw: record)
        self.engine = patch.object(pub.release_verify, "verify_release", return_value={"verified": True})
        self.validator.start()
        self.verify = self.engine.start()
        self.addCleanup(self.validator.stop)
        self.addCleanup(self.engine.stop)

    def prepare(self):
        return self.publisher.prepare()

    def draft(self, *, published=False):
        release = {"id": 10, "tag_name": TAG, "target_commitish": SHA, "name": TAG, "body": NOTES,
                   "draft": not published, "prerelease": False}
        self.api.releases = [release]
        if published:
            self.api.latest = release
        return release

    def upload_all(self):
        for name in self.publisher.names:
            self.api.upload(10, self.publisher.tag_dir / name)
        self.api.mutations.clear()

    def blocked(self):
        with self.assertRaises((pub.ReleaseError, pub.release_verify.VerificationError)):
            self.prepare()
        self.assertEqual(self.api.mutations, [])

    def test_prepare_is_read_only_and_checks_all_three_builds(self):
        report = self.prepare()
        self.assertEqual(report["tag_run_id"], 13)
        self.assertEqual(len(report["assets_sha256"]), 11)
        self.assertEqual(len(self.api.downloads), 4)
        self.verify.assert_called_once()
        self.assertEqual(self.api.mutations, [])

    def test_create_draft_verify_then_publish_and_redownload(self):
        self.prepare()
        outcome = self.publisher.publish()
        self.assertEqual(outcome["state"], "published-and-redownload-verified")
        methods = [call[0] for call in self.api.mutations]
        self.assertEqual(methods, ["POST"] + ["UPLOAD"] * 11 + ["PATCH"])
        self.assertEqual(self.api.mutations[-1][2], {"draft": False, "make_latest": "true"})
        self.assertEqual(len([path for path in self.api.downloads if "/releases/assets/" in path]), 22)

    def test_resume_partial_draft_fills_only_missing_files(self):
        self.prepare()
        self.draft()
        name = self.publisher.names[0]
        self.api.upload(10, self.publisher.tag_dir / name)
        self.api.mutations.clear()
        self.publisher.publish()
        self.assertEqual(len([x for x in self.api.mutations if x[0] == "UPLOAD"]), 10)
        self.assertNotIn(("UPLOAD", 10, name), self.api.mutations)
        self.assertFalse(any(x[0] == "POST" for x in self.api.mutations))

    def test_published_retry_is_strictly_read_only(self):
        self.prepare()
        self.draft(published=True)
        self.upload_all()
        result = self.publisher.publish()
        self.assertEqual(result["state"], "already-published-verified")
        self.assertEqual(self.api.mutations, [])

    def test_conflicting_draft_asset_blocks_without_overwrite(self):
        self.prepare()
        self.draft()
        self.upload_all()
        asset = self.api.assets[0]
        self.api.bytes[f"{PREFIX}/releases/assets/{asset['id']}"] = b"corrupt"
        with self.assertRaisesRegex(pub.ReleaseError, "bytes conflict"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_published_missing_asset_never_repaired(self):
        self.prepare()
        self.draft(published=True)
        with self.assertRaisesRegex(pub.ReleaseError, "missing expected"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_conflicting_release_metadata_blocks(self):
        self.prepare()
        self.draft()["body"] = "unapproved notes"
        with self.assertRaisesRegex(pub.ReleaseError, "conflicts"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_unexpected_assets_block_before_upload(self):
        self.prepare()
        self.draft()
        self.api.assets = [{"id": 500, "name": "surprise.txt"}]
        with self.assertRaisesRegex(pub.ReleaseError, "unexpected assets"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_newer_latest_blocks_downgrade(self):
        self.prepare()
        self.api.latest = {"tag_name": "v2.0.0"}
        with self.assertRaisesRegex(pub.ReleaseError, "newer Latest"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_asset_added_after_draft_verification_blocks_publication(self):
        self.prepare()
        original = self.publisher.verify_assets
        def race(release, label, **kwargs):
            missing = original(release, label, **kwargs)
            if label == "verified-draft":
                self.api.assets.append({"id": 9999, "name": "surprise.txt", "state": "uploaded", "size": 1})
            return missing
        with patch.object(self.publisher, "verify_assets", side_effect=race):
            with self.assertRaisesRegex(pub.ReleaseError, "inventory changed"):
                self.publisher.publish()
        self.assertFalse(any(call[0] == "PATCH" for call in self.api.mutations))

    def test_wrong_candidate_sha_blocks(self):
        self.api.runs[11]["head_sha"] = "9" * 40
        self.blocked()

    def test_candidate_rerun_attempt_blocks(self):
        self.api.runs[11]["run_attempt"] = 2
        self.blocked()

    def test_wrong_workflow_id_blocks(self):
        self.api.runs[11]["workflow_id"] = 999
        self.blocked()

    def test_wrong_workflow_path_blocks(self):
        self.api.runs[11]["path"] = pub.WORKFLOW + ".evil"
        self.blocked()

    def test_exact_ref_qualified_workflow_paths_supported(self):
        self.api.runs[11]["path"] += "@refs/heads/main"
        self.api.runs[13]["path"] += f"@refs/tags/{TAG}"
        self.prepare()

    def test_pull_request_candidate_blocks(self):
        self.api.runs[11]["event"] = "pull_request"
        self.blocked()

    def test_fork_candidate_blocks(self):
        self.api.runs[11]["head_repository"] = {"id": 9, "full_name": "attacker/px4-userland", "fork": True}
        self.blocked()

    def test_failed_tag_run_blocks_before_artifact_download(self):
        self.api.runs[13]["conclusion"] = "failure"
        self.blocked()
        self.assertEqual(self.api.downloads, [])

    def test_missing_run_times_out_finitely(self):
        self.api.listed_runs = []
        clock = [0]
        self.publisher.timeout = 2
        self.publisher.poll_interval = 1
        self.publisher.monotonic = lambda: clock[0]
        self.publisher.sleep = lambda amount: clock.__setitem__(0, clock[0] + amount)
        self.blocked()
        self.assertEqual(clock[0], 2)

    def test_queued_run_waits_for_all_jobs_to_complete(self):
        self.api.runs[13].update(status="in_progress", conclusion=None)
        def finish(_):
            self.api.runs[13].update(status="completed", conclusion="success")
        self.publisher.sleep = finish
        self.prepare()

    def test_duplicate_tag_run_blocks(self):
        self.api.listed_runs.append(copy.deepcopy(self.api.runs[13]))
        self.blocked()

    def test_tag_moved_before_publication_blocks(self):
        self.prepare()
        self.api.tag_target = "9" * 40
        with self.assertRaisesRegex(pub.ReleaseError, "event commit"):
            self.publisher.publish()
        self.assertEqual(self.api.mutations, [])

    def test_reannotated_same_commit_blocks(self):
        self.prepare()
        self.api.tag_object = "9" * 40
        with self.assertRaisesRegex(pub.ReleaseError, "tag changed"):
            self.publisher.publish()

    def test_lightweight_tag_blocks(self):
        self.api.tag_type = "commit"
        self.blocked()

    def test_nonmain_tag_blocks(self):
        self.api.compare_status = "diverged"
        self.blocked()

    def test_recorded_git_tree_must_match_actual_commit(self):
        self.api.commits[SOURCE_SHA]["tree"]["sha"] = "9" * 40
        self.blocked()

    def test_export_ignored_payload_change_still_blocks(self):
        self.api.trees[TREE]["tree"][1]["sha"] = "9" * 40
        self.blocked()

    def test_document_mode_change_blocks(self):
        self.api.trees[TREE]["tree"][0]["mode"] = "100755"
        self.blocked()

    def test_truncated_git_tree_blocks(self):
        self.api.trees[TREE]["truncated"] = True
        self.blocked()

    def test_deleted_git_file_blocks(self):
        self.api.trees[TREE]["tree"].pop()
        self.blocked()

    def test_wrong_artifact_id_blocks(self):
        raw = self.record
        raw["candidate"]["artifact_id"] = 900
        self.api.documents[f"release-notes/{TAG}.json"] = json.dumps(raw).encode()
        self.blocked()

    def test_expired_artifact_blocks(self):
        self.api.artifacts[111]["expired"] = True
        self.blocked()

    def test_past_expiry_time_blocks_even_if_flag_false(self):
        self.api.artifacts[111]["expires_at"] = "2000-01-01T00:00:00Z"
        self.blocked()

    def test_wrong_artifact_run_identity_blocks(self):
        self.api.artifacts[111]["workflow_run"]["id"] = 999
        self.blocked()

    def test_wrong_artifact_head_repository_blocks(self):
        self.api.artifacts[111]["workflow_run"]["head_repository_id"] = 999
        self.blocked()

    def test_missing_artifact_digest_blocks(self):
        self.api.artifacts[111].pop("digest")
        self.blocked()

    def test_bad_archive_digest_blocks(self):
        self.api.artifacts[111]["digest"] = "sha256:" + "0" * 64
        self.blocked()

    def test_duplicate_named_artifact_blocks(self):
        self.api.run_artifacts[11].append(copy.deepcopy(self.api.run_artifacts[11][0]))
        self.blocked()

    def test_same_named_branch_is_not_accepted_as_tag_build(self):
        raw = {"schema": 1, "repository": REPOSITORY, "event": "push", "ref": f"refs/heads/{TAG}",
               "sha": SHA, "run_id": 13, "run_attempt": 1}
        archive = zip_bytes({"release-provenance.json": json.dumps(raw).encode()})
        self.api.artifacts[114]["digest"] = "sha256:" + hashlib.sha256(archive).hexdigest()
        self.api.bytes[f"{PREFIX}/actions/artifacts/114/zip"] = archive
        self.blocked()

    def test_offline_failure_has_no_release_side_effects(self):
        self.verify.side_effect = pub.release_verify.VerificationError("binary changed")
        self.blocked()


class EventTests(unittest.TestCase):
    def setUp(self):
        self.env = {"GITHUB_EVENT_NAME": "push", "GITHUB_REPOSITORY": REPOSITORY,
                    "GITHUB_REF": f"refs/tags/{TAG}", "GITHUB_SHA": SHA}
        self.event = {"repository": {"full_name": REPOSITORY, "fork": False},
                      "ref": self.env["GITHUB_REF"], "deleted": False}

    def test_valid_tag_event(self):
        self.assertEqual(pub.validate_event(self.env, self.event, "px4-userland"), (REPOSITORY, TAG, SHA))

    def test_other_events_rejected(self):
        for event in ("pull_request", "pull_request_target", "workflow_run", "workflow_dispatch"):
            with self.subTest(event=event), self.assertRaises(pub.ReleaseError):
                pub.validate_event({**self.env, "GITHUB_EVENT_NAME": event}, self.event, "px4-userland")

    def test_invalid_tag_names_rejected(self):
        for tag in ("v1.2", "v01.2.3", "v1.2.3-rc1", "v1.2.3\n", "v1.2.3/evil"):
            ref = "refs/tags/" + tag
            with self.subTest(tag=tag), self.assertRaises(pub.ReleaseError):
                pub.validate_event({**self.env, "GITHUB_REF": ref}, {**self.event, "ref": ref}, "px4-userland")

    def test_fork_wrong_product_deleted_and_mismatched_ref_rejected(self):
        for change in ({"repository": {"full_name": REPOSITORY, "fork": True}}, {"deleted": True},
                       {"ref": "refs/heads/main"}, {"repository": {"full_name": "x/y", "fork": False}}):
            with self.subTest(change=change), self.assertRaises(pub.ReleaseError):
                pub.validate_event(self.env, {**self.event, **change}, "px4-userland")
        with self.assertRaises(pub.ReleaseError):
            pub.validate_event(self.env, self.event, "asicen-userland")


class HTTPTests(unittest.TestCase):
    def test_api_paths_are_repository_scoped(self):
        api = pub.GitHub(REPOSITORY, "secret")
        for path in ("https://evil.test/", "//evil.test/", "/repos/other/repo/releases", PREFIX + "/../evil",
                     PREFIX + "/%2e%2e/evil", PREFIX + "/releases\nAuthorization: x"):
            with self.subTest(path=path), self.assertRaises(pub.ReleaseError):
                api.api_url(path)

    def test_unsafe_redirects_rejected(self):
        for url in ("http://release-assets.githubusercontent.com/file", "https://evil.test/file",
                    "https://release-assets.githubusercontent.com.evil.test/file", "https://127.0.0.1/file",
                    "https://user:pass@release-assets.githubusercontent.com/file",
                    "https://release-assets.githubusercontent.com:8443/file", "file:///tmp/secret"):
            with self.subTest(url=url), self.assertRaises(pub.ReleaseError):
                pub.safe_download_url(url)

    def test_redirect_download_never_forwards_token(self):
        requests = []
        class Opener:
            def open(self, request, timeout):
                requests.append(request)
                if len(requests) == 1:
                    raise HTTPError(request.full_url, 302, "Found", {"Location": "https://release-assets.githubusercontent.com/file?sig=xyz"}, None)
                return BytesIO(b"payload")
        api = pub.GitHub(REPOSITORY, "secret", opener=Opener())
        with tempfile.TemporaryDirectory() as directory:
            api.download(PREFIX + "/releases/assets/1", Path(directory) / "asset")
        self.assertEqual(requests[0].get_header("Authorization"), "Bearer secret")
        self.assertIsNone(requests[1].get_header("Authorization"))

    def test_mutation_not_retried_on_server_failure(self):
        calls = []
        class Opener:
            def open(self, request, timeout):
                calls.append(request)
                raise HTTPError(request.full_url, 502, "Bad Gateway", {}, None)
        api = pub.GitHub(REPOSITORY, "secret", opener=Opener())
        with self.assertRaises(pub.ReleaseError):
            api.json("POST", PREFIX + "/releases", {"draft": True})
        self.assertEqual(len(calls), 1)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(pub.ReleaseError):
            pub.parse_json('{"draft": true, "draft": false}')

    def test_artifact_zip_traversal_and_symlinks_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "artifact.zip"
            path.write_bytes(zip_bytes({"../escape": b"bad"}))
            with self.assertRaises(pub.ReleaseError):
                pub.extract_artifact(path, root / "traversal", ("../escape",))
            with zipfile.ZipFile(path, "w") as archive:
                member = zipfile.ZipInfo("asset")
                member.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(member, b"../escape")
            with self.assertRaises(pub.ReleaseError):
                pub.extract_artifact(path, root / "link", ("asset",))


class FailureReportingTests(unittest.TestCase):
    def test_uncertain_publication_keeps_machine_readable_state_and_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event = root / "event.json"
            event.write_text(json.dumps({"repository": {"full_name": REPOSITORY, "fork": False},
                                         "ref": f"refs/tags/{TAG}", "deleted": False}))
            report = root / "report.json"
            publisher = MagicMock()
            publisher.prepare.return_value = {"tag": TAG, "tag_sha": SHA}
            publisher.publish.side_effect = pub.ReleaseError("connection lost after publication request")
            publisher.phase = "publish-latest"
            publisher.release_id = 10
            publisher.publication_attempted = True
            env = {"GITHUB_EVENT_PATH": str(event), "GITHUB_EVENT_NAME": "push", "GITHUB_REPOSITORY": REPOSITORY,
                   "GITHUB_REF": f"refs/tags/{TAG}", "GITHUB_SHA": SHA, "GITHUB_TOKEN": "token"}
            stderr = StringIO()
            with patch.dict("os.environ", env, clear=True), patch.object(pub, "Publisher", return_value=publisher), redirect_stderr(stderr):
                result = pub.main(["publish", "--product", "px4-userland", "--report", str(report)])
            self.assertEqual(result, 1)
            value = json.loads(report.read_text())
            self.assertTrue(value["publication_attempted"])
            self.assertEqual(value["release_id"], 10)
            self.assertEqual(value["status"], "blocked")
            self.assertIn("may already be public", stderr.getvalue())


class WorkflowTests(unittest.TestCase):
    def test_workflow_privilege_and_trigger_contract(self):
        root = Path(__file__).resolve().parents[1]
        # Preserve upstream cross-repository contract tests against its frozen
        # workflows; test_release_local.py tests ASICEN's actual local policy.
        upstream = root / "tests/fixtures/release-upstream"
        wrapper = (upstream / "release_tag.yml").read_text()
        shared = (upstream / "release_reusable.yml").read_text()
        build = (root / ".github/workflows/build_userland.yml").read_text()
        self.assertIn("cancel-in-progress: false", wrapper)
        self.assertIn("verifier-ref: ${{ github.sha }}", wrapper)
        self.assertNotIn("workflow_run:", wrapper + shared)
        self.assertNotIn("workflow_dispatch:", wrapper + shared)
        self.assertNotIn("environment:", shared)
        self.assertNotIn("secrets: inherit", wrapper + shared)
        verify, publish = shared.split("  publish:\n")
        self.assertIn("contents: read", verify)
        self.assertNotIn("contents: write", verify)
        self.assertIn("contents: write", publish)
        self.assertIn("needs: verify", publish)
        self.assertEqual(shared.count("persist-credentials: false"), 2)
        self.assertNotIn("contents: write", build)
        self.assertIn("'ref': os.environ['GITHUB_REF']", build)
        self.assertIn("name: release-provenance", build)

    def bootstrap(self, repository, revision, references=None):
        root = Path(__file__).resolve().parents[1]
        shared = (root / "tests/fixtures/release-upstream/release_reusable.yml").read_text()
        source = shared.split("          python3 - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
        source = textwrap.dedent(source)
        run = {"id": 42, "head_sha": SHA, "repository": {"full_name": repository}, "event": "push",
               "referenced_workflows": references or []}
        requests = []
        class Opener:
            def open(self, request, timeout):
                requests.append(request)
                return BytesIO(json.dumps(run).encode())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            env = {"VERIFIER_REF": revision, "GITHUB_REPOSITORY": repository, "GITHUB_SHA": SHA,
                   "GITHUB_TOKEN": "test-token", "GITHUB_RUN_ID": "42", "GITHUB_OUTPUT": str(output)}
            with patch.dict("os.environ", env, clear=True), patch("urllib.request.build_opener", return_value=Opener()):
                exec(compile(source, "workflow-bootstrap", "exec"), {})
            self.assertEqual(output.read_text(), f"sha={revision}\n")
        return requests

    def test_local_bootstrap_requires_same_commit(self):
        self.assertEqual(self.bootstrap(REPOSITORY, SHA), [])
        with self.assertRaises(SystemExit):
            self.bootstrap(REPOSITORY, SOURCE_SHA)

    def test_cross_repo_bootstrap_proves_actual_reusable_workflow_pin(self):
        path = "Khronos31/px4-userland/.github/workflows/release_reusable.yml@" + SOURCE_SHA
        requests = self.bootstrap("Khronos31/asicen-userland", SOURCE_SHA, [{"path": path, "sha": SOURCE_SHA}])
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0].full_url, "https://api.github.com/repos/Khronos31/asicen-userland/actions/runs/42")

    def test_cross_repo_bootstrap_rejects_missing_mismatched_or_floating_pin(self):
        path = "Khronos31/px4-userland/.github/workflows/release_reusable.yml@"
        for references in ([], [{"path": path + SOURCE_SHA, "sha": SHA}],
                           [{"path": path + "main", "sha": SOURCE_SHA}]):
            with self.subTest(references=references), self.assertRaises(SystemExit):
                self.bootstrap("Khronos31/asicen-userland", SOURCE_SHA, references)

    def test_bootstrap_rejects_nonimmutable_revision_and_unknown_caller(self):
        with self.assertRaises(SystemExit):
            self.bootstrap(REPOSITORY, "main")
        with self.assertRaises(SystemExit):
            self.bootstrap("attacker/asicen-userland", SHA)


if __name__ == "__main__":
    unittest.main()
