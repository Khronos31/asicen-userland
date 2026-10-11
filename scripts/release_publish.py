#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail-closed GitHub boundary for the shared Stable release verifier.

Only ``publish`` mutates GitHub. The workflow runs ``verify`` with a read-only
token first, then repeats verification in the sole write-permission job. No
code, shell command, URL, or executable from a build artifact is ever executed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import time
from datetime import datetime, timezone
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import zipfile

import release_verify


REPOSITORIES = {
    "px4-userland": "Khronos31/px4-userland",
    "asicen-userland": "Khronos31/asicen-userland",
}
# Reviewed release targets, never supplied by a record or an artifact. Changing
# ASICEN's current release line requires a normal reviewed implementation change.
RELEASE_BRANCHES = {"px4-userland": "main", "asicen-userland": "release/0.1.0"}
WORKFLOW = ".github/workflows/build_userland.yml"
ARTIFACT = "release-candidate"
TAG_RE = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
SHA_RE = re.compile(r"[0-9a-f]{40}\Z")
MAX_DOWNLOAD = 2 * 1024**3
MAX_JSON = 16 * 1024**2
MAX_PAGES = 100


class ReleaseError(RuntimeError):
    """A release gate failed. Never continue to publication after this error."""


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def positive_id(value, label):
    require(type(value) is int and value > 0, f"invalid {label}")
    return value


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024**2), b""):
            digest.update(block)
    return digest.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(raw):
    try:
        return json.loads(raw, object_pairs_hook=unique_object)
    except (ValueError, UnicodeError) as exc:
        raise ReleaseError("invalid JSON") from exc


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def safe_download_url(url):
    """Signed download URLs are followed without Authorization, on HTTPS only."""
    parsed = urlsplit(url)
    host = parsed.hostname or ""
    require(parsed.scheme == "https" and not parsed.username and not parsed.password
            and parsed.port in (None, 443) and not parsed.fragment,
            "unsafe artifact redirect URL")
    require(host in {"release-assets.githubusercontent.com", "objects.githubusercontent.com"}
            or host.endswith(".blob.core.windows.net")
            or host.endswith(".actions.githubusercontent.com"),
            "artifact redirect host is not an approved GitHub download host")
    require(not any(ord(char) < 32 for char in url), "control character in URL")
    return url


class GitHub:
    """Small REST client: fixed API origins, no credential-bearing redirects.

    Mutating requests are deliberately never retried automatically. A rerun
    observes server state and only fills missing, verified draft assets.
    """

    def __init__(self, repository, token, *, opener=None):
        require(repository in REPOSITORIES.values(), "repository is not an approved product")
        require(bool(token), "GITHUB_TOKEN is required")
        self.repository = repository
        self.prefix = f"/repos/{repository}"
        self.token = token
        self.opener = opener or build_opener(NoRedirect())

    def api_url(self, path):
        require(path.startswith(self.prefix + "/") or path == self.prefix,
                "API path is outside the release repository")
        parsed = urlsplit(path)
        require(not parsed.scheme and not parsed.netloc and not parsed.fragment
                and not any(ord(c) < 32 for c in path)
                and ".." not in parsed.path and "%" not in parsed.path,
                "unsafe API path")
        return "https://api.github.com" + path

    def request(self, method, url, *, data=None, authenticated=True,
                content_type="application/json", accept="application/vnd.github+json"):
        headers = {"Accept": accept, "User-Agent": "userland-stable-release",
                   "X-GitHub-Api-Version": "2022-11-28"}
        if authenticated:
            parsed = urlsplit(url)
            require(parsed.scheme == "https" and parsed.netloc in
                    {"api.github.com", "uploads.github.com"}, "unsafe authenticated API origin")
            headers["Authorization"] = f"Bearer {self.token}"
        if data is not None:
            headers["Content-Type"] = content_type
        request = Request(url, data=data, headers=headers, method=method)
        try:
            return self.opener.open(request, timeout=60)
        except HTTPError:
            raise
        except (URLError, TimeoutError, OSError) as exc:
            # Do not echo signed URLs, credentials, or response bodies to logs.
            raise ReleaseError(f"GitHub {method} request failed; rerun after checking server state") from exc

    def json(self, method, path, body=None, *, missing_ok=False):
        payload = None if body is None else json.dumps(body).encode("utf-8")
        try:
            response = self.request(method, self.api_url(path), data=payload)
        except HTTPError as exc:
            if missing_ok and method == "GET" and exc.code == 404:
                return None
            raise ReleaseError(f"GitHub {method} failed with HTTP {exc.code}") from None
        with response:
            raw = response.read(MAX_JSON + 1)
        require(len(raw) <= MAX_JSON, "GitHub JSON response exceeds safety limit")
        return parse_json(raw)

    def list(self, path, key=None):
        """Pagination uses our own same-repository path, never a response Link URL."""
        results = []
        for page in range(1, MAX_PAGES + 1):
            separator = "&" if "?" in path else "?"
            value = self.json("GET", f"{path}{separator}per_page=100&page={page}")
            entries = value if key is None else value.get(key)
            require(isinstance(entries, list), "invalid paginated GitHub response")
            results.extend(entries)
            if len(entries) < 100:
                if key is not None and "total_count" in value:
                    require(len(results) == value["total_count"], "incomplete or racing API listing")
                return results
        raise ReleaseError("GitHub pagination limit reached; refusing incomplete inventory")

    def download(self, path, destination):
        url = self.api_url(path)
        authenticated = True
        for _ in range(6):
            try:
                response = self.request("GET", url, authenticated=authenticated,
                                        accept="application/octet-stream")
            except HTTPError as exc:
                if exc.code in (301, 302, 303, 307, 308):
                    url = safe_download_url(exc.headers.get("Location", ""))
                    authenticated = False
                    continue
                raise ReleaseError(f"GitHub download failed with HTTP {exc.code}") from None
            total = 0
            with response, Path(destination).open("xb") as output:
                while True:
                    block = response.read(1024**2)
                    if not block:
                        break
                    total += len(block)
                    require(total <= MAX_DOWNLOAD, "download exceeds safety limit")
                    output.write(block)
            return
        raise ReleaseError("too many download redirects")

    def upload(self, release_id, file):
        positive_id(release_id, "release id")
        file = Path(file)
        require(file.is_file() and not file.is_symlink(), "unsafe asset upload input")
        require(file.stat().st_size <= MAX_DOWNLOAD, "upload exceeds safety limit")
        # Construct this URL; do not trust upload_url in the release response.
        url = (f"https://uploads.github.com{self.prefix}/releases/{release_id}/assets?"
               + urlencode({"name": file.name}))
        try:
            response = self.request("POST", url, data=file.read_bytes(),
                                    content_type="application/octet-stream")
        except HTTPError as exc:
            raise ReleaseError(f"GitHub asset upload failed with HTTP {exc.code}") from None
        with response:
            raw = response.read(MAX_JSON + 1)
        require(len(raw) <= MAX_JSON, "upload response exceeds safety limit")
        return parse_json(raw)


def validate_event(env, event, product):
    require(product in REPOSITORIES, "unsupported product")
    repository = REPOSITORIES[product]
    require(env.get("GITHUB_EVENT_NAME") == "push", "only a tag push may release")
    require(env.get("GITHUB_REPOSITORY") == repository, "unexpected release repository")
    require(event.get("repository", {}).get("full_name") == repository
            and event.get("repository", {}).get("fork") is False,
            "fork or wrong-repository event")
    ref = env.get("GITHUB_REF", "")
    require(ref.startswith("refs/tags/") and event.get("ref") == ref,
            "release event must identify the same tag ref")
    require(event.get("deleted") is False, "deleted tag cannot release")
    tag = ref[len("refs/tags/"):]
    require(bool(TAG_RE.fullmatch(tag)), "only stable vX.Y.Z tags may release")
    require(bool(SHA_RE.fullmatch(env.get("GITHUB_SHA", ""))), "invalid event commit SHA")
    return repository, tag, env["GITHUB_SHA"]


def extract_artifact(zip_path, destination, expected):
    """The Actions wrapper ZIP must contain only the exact flat asset set."""
    destination = Path(destination)
    destination.mkdir()
    try:
        with zipfile.ZipFile(zip_path) as archive:
            members = archive.infolist()
            names = [item.filename for item in members]
            require(len(names) == len(set(names)) and set(names) == set(expected),
                    "Actions artifact asset inventory is not exact")
            total = 0
            for member in members:
                require(member.filename == Path(member.filename).name
                        and not member.is_dir() and not member.flag_bits & 1,
                        "unsafe Actions artifact entry")
                mode = member.external_attr >> 16
                require(stat.S_IFMT(mode) in (0, stat.S_IFREG),
                        "Actions artifact contains a nonregular file")
                total += member.file_size
                require(total <= MAX_DOWNLOAD, "unpacked artifact exceeds safety limit")
                with archive.open(member) as source, (destination / member.filename).open("xb") as output:
                    while True:
                        block = source.read(1024**2)
                        if not block:
                            break
                        output.write(block)
    except (zipfile.BadZipFile, OSError) as exc:
        raise ReleaseError("invalid Actions artifact ZIP") from exc


class Publisher:
    def __init__(self, api, product, tag, tag_sha, directory, *, timeout=10800,
                 poll_interval=30, sleep=time.sleep, monotonic=time.monotonic):
        require(product in REPOSITORIES and api.repository == REPOSITORIES[product],
                "product/repository mismatch")
        require(bool(TAG_RE.fullmatch(tag)) and bool(SHA_RE.fullmatch(tag_sha)),
                "invalid tag or tag SHA")
        require(0 < timeout <= 14400 and 0 < poll_interval <= 300, "invalid polling bounds")
        self.api = api
        self.product = product
        self.tag = tag
        self.sha = tag_sha
        self.root = Path(directory)
        self.timeout = timeout
        self.poll_interval = poll_interval
        self.sleep = sleep
        self.monotonic = monotonic
        self.prefix = api.prefix
        self.names = release_verify.expected_assets(product, tag[1:])
        self.tag_object = None
        self.repository_id = None
        self.workflow_id = None
        self.phase = "verify-inputs"
        self.publication_attempted = False
        self.release_id = None

    def check_tag(self):
        ref = self.api.json("GET", f"{self.prefix}/git/ref/tags/{self.tag}")
        require(ref.get("ref") == f"refs/tags/{self.tag}", "wrong tag reference")
        obj = ref.get("object", {})
        require(obj.get("type") == "tag" and SHA_RE.fullmatch(obj.get("sha", "")),
                "release requires an annotated tag")
        tag = self.api.json("GET", f"{self.prefix}/git/tags/{obj['sha']}")
        require(tag.get("sha") == obj["sha"] and tag.get("tag") == self.tag
                and tag.get("object", {}).get("type") == "commit"
                and tag["object"].get("sha") == self.sha,
                "tag does not resolve to the event commit")
        if self.tag_object is None:
            self.tag_object = obj["sha"]
        require(self.tag_object == obj["sha"], "tag changed during release")
        return tag

    def file_at_tag(self, path):
        result = self.api.json("GET", f"{self.prefix}/contents/{path}?ref={self.sha}")
        require(result.get("type") == "file" and result.get("path") == path
                and result.get("encoding") == "base64", "invalid committed release input")
        try:
            raw = base64.b64decode(result["content"].replace("\n", ""), validate=True)
        except (KeyError, ValueError) as exc:
            raise ReleaseError("invalid committed file encoding") from exc
        require(len(raw) == result.get("size") and len(raw) <= MAX_JSON,
                "invalid committed file size")
        return raw

    def check_run(self, run, sha, *, run_id=None, attempt=None, tag=False, complete=True):
        positive_id(run.get("id"), "workflow run id")
        if run_id is not None:
            require(run["id"] == run_id, "wrong workflow run id")
        branch = run.get("head_branch", "")
        require(isinstance(branch, str) and branch and not any(ord(c) < 32 for c in branch),
                "invalid build ref name")
        refs = {sha, branch, f"refs/tags/{self.tag}"} if tag else {sha, branch, f"refs/heads/{branch}"}
        paths = {WORKFLOW} | {f"{WORKFLOW}@{ref}" for ref in refs}
        require(run.get("workflow_id") == self.workflow_id
                and run.get("path") in paths, "wrong build workflow identity")
        for key in ("repository", "head_repository"):
            repo = run.get(key, {})
            require(repo.get("full_name") == self.api.repository and repo.get("id") == self.repository_id
                    and repo.get("fork") is False, "workflow run is from a fork or different repository")
        require(run.get("head_sha") == sha, "wrong workflow run commit")
        require(run.get("event") in ("push", "workflow_dispatch"), "untrusted candidate event")
        require(not run.get("pull_requests"), "PR-associated workflow run cannot release")
        if tag:
            require(run.get("event") == "push" and run.get("head_branch") == self.tag,
                    "run is not the exact tag push build")
        positive_id(run.get("run_attempt"), "workflow run attempt")
        if attempt is not None:
            require(run["run_attempt"] == attempt, "run attempt changed since candidate validation")
        if complete:
            require(run.get("status") == "completed" and run.get("conclusion") == "success",
                    "build workflow has not completed successfully")
        return run

    def candidate_run(self, record):
        run_id, attempt = record["run_id"], record["run_attempt"]
        current = self.api.json("GET", f"{self.prefix}/actions/runs/{run_id}")
        self.check_run(current, record["sha"], run_id=run_id, attempt=attempt)
        exact = self.api.json("GET", f"{self.prefix}/actions/runs/{run_id}/attempts/{attempt}")
        self.check_run(exact, record["sha"], run_id=run_id, attempt=attempt)
        return current

    def git_inventory(self, tree):
        result = self.api.json("GET", f"{self.prefix}/git/trees/{tree}?recursive=1")
        require(result.get("sha") == tree and result.get("truncated") is False,
                "Git tree inventory is incomplete or has wrong identity")
        entries = result.get("tree")
        require(isinstance(entries, list), "invalid Git tree inventory")
        inventory = {}
        seen = set()
        for entry in entries:
            path = entry.get("path")
            release_verify.safe_name(path)
            require(path not in seen, "duplicate Git tree path")
            seen.add(path)
            require(SHA_RE.fullmatch(entry.get("sha", "")), "invalid Git entry identity")
            kind, mode = entry.get("type"), entry.get("mode")
            require((kind == "tree" and mode == "040000") or
                    (kind == "blob" and mode in {"100644", "100755", "120000"}) or
                    (kind == "commit" and mode == "160000"), "invalid Git tree entry type/mode")
            if kind != "tree":
                inventory[path] = (kind, mode, entry["sha"])
        return inventory

    def verify_git_difference(self):
        candidate = self.record["candidate"]
        for name in ("candidate", "reproduction"):
            record = self.record[name]
            commit = self.api.json("GET", f"{self.prefix}/git/commits/{record['sha']}")
            require(commit.get("sha") == record["sha"] and commit.get("tree", {}).get("sha") == record["tree"],
                    f"{name} recorded Git tree does not match its source commit")
        before, after = self.git_inventory(candidate["tree"]), self.git_inventory(self.tree)
        require(set(before) == set(after), "candidate/tag Git file inventory differs")
        allowed = release_verify.allowed_documents(self.product, self.tag)
        for path in before:
            if before[path] == after[path]:
                continue
            require(path in allowed and before[path][:2] == after[path][:2] == ("blob", "100644"),
                    f"non-documentation Git change since tested candidate: {path}")

    def wait_tag_run(self):
        deadline = self.monotonic() + self.timeout
        while True:
            self.check_tag()
            query = urlencode({"head_sha": self.sha, "event": "push", "exclude_pull_requests": "true"})
            runs = self.api.list(f"{self.prefix}/actions/workflows/{self.workflow_id}/runs?{query}",
                                 "workflow_runs")
            matches = [run for run in runs if run.get("head_branch") == self.tag]
            require(len(matches) <= 1, "multiple build runs claim this tag; refusing ambiguous provenance")
            if matches:
                run = self.check_run(matches[0], self.sha, tag=True, complete=False)
                if run.get("status") == "completed":
                    return self.check_run(run, self.sha, tag=True)
                require(run.get("status") in {"queued", "in_progress", "waiting", "pending", "requested"},
                        "tag build is in an unsupported state")
            remaining = deadline - self.monotonic()
            require(remaining > 0, "timed out waiting for the exact successful tag build")
            self.sleep(min(self.poll_interval, remaining))

    def artifact(self, run, label, expected_id=None, *, name=ARTIFACT, expected=None):
        run_id = run["id"]
        entries = self.api.list(f"{self.prefix}/actions/runs/{run_id}/artifacts", "artifacts")
        matches = [item for item in entries if item.get("name") == name]
        require(len(matches) == 1, "missing or duplicate release-candidate artifact")
        listed = matches[0]
        artifact_id = positive_id(listed.get("id"), "artifact id")
        if expected_id is not None:
            require(artifact_id == expected_id, "candidate artifact id does not match reviewed record")
        artifact = self.api.json("GET", f"{self.prefix}/actions/artifacts/{artifact_id}")
        require(artifact.get("id") == artifact_id and artifact.get("name") == name,
                "artifact identity changed")
        require(artifact.get("expired") is False, "candidate artifact is expired")
        try:
            expiry = datetime.fromisoformat(artifact["expires_at"].replace("Z", "+00:00"))
            require(expiry > datetime.now(timezone.utc), "artifact expiry time has passed")
        except (KeyError, ValueError, TypeError) as exc:
            raise ReleaseError("invalid artifact expiry") from exc
        identity = artifact.get("workflow_run", {})
        require(identity.get("id") == run_id and identity.get("head_sha") == run["head_sha"]
                and identity.get("repository_id") == self.repository_id
                and identity.get("head_repository_id") == self.repository_id,
                "artifact is not bound to the verified build run")
        digest = artifact.get("digest")
        require(isinstance(digest, str) and re.fullmatch(r"sha256:[0-9a-f]{64}", digest),
                "artifact has no immutable SHA-256 digest")
        zip_path = self.root / f"{label}.zip"
        self.api.download(f"{self.prefix}/actions/artifacts/{artifact_id}/zip", zip_path)
        require("sha256:" + sha256(zip_path) == digest, "Actions artifact ZIP digest mismatch")
        folder = self.root / label
        extract_artifact(zip_path, folder, self.names if expected is None else expected)
        # Re-fetch after download so a rerun/deletion cannot silently replace provenance.
        current = self.api.json("GET", f"{self.prefix}/actions/runs/{run_id}")
        self.check_run(current, run["head_sha"], run_id=run_id, attempt=run["run_attempt"],
                       tag=(run["id"] == self.tag_run["id"]))
        return folder, artifact_id

    def tag_provenance(self):
        folder, _ = self.artifact(self.tag_run, "tag-provenance", name="release-provenance",
                                 expected=("release-provenance.json",))
        provenance = parse_json((folder / "release-provenance.json").read_bytes())
        expected = {"schema": 1, "repository": self.api.repository, "event": "push",
                    "ref": f"refs/tags/{self.tag}", "sha": self.sha,
                    "run_id": self.tag_run["id"], "run_attempt": self.tag_run["run_attempt"]}
        require(provenance == expected, "build provenance is not the exact tag push event")

    def prepare(self):
        repository = self.api.json("GET", self.prefix)
        require(repository.get("full_name") == self.api.repository and repository.get("fork") is False,
                "unexpected repository identity")
        self.repository_id = positive_id(repository.get("id"), "repository id")
        self.check_tag()
        branch = RELEASE_BRANCHES[self.product]
        reference = self.api.json("GET", f"{self.prefix}/git/ref/heads/{branch}")
        target = reference.get("object", {})
        require(reference.get("ref") == f"refs/heads/{branch}" and target.get("type") == "commit"
                and SHA_RE.fullmatch(target.get("sha", "")), "wrong release branch identity")
        ancestry = self.api.json("GET", f"{self.prefix}/compare/{self.sha}...{target['sha']}")
        require(ancestry.get("status") in {"ahead", "identical"},
                f"tag commit is not on the reviewed release branch: {branch}")
        commit = self.api.json("GET", f"{self.prefix}/git/commits/{self.sha}")
        require(commit.get("sha") == self.sha and SHA_RE.fullmatch(commit.get("tree", {}).get("sha", "")),
                "invalid tag commit/tree identity")
        self.tree = commit["tree"]["sha"]
        require(self.file_at_tag("VERSION").decode("ascii").strip() == self.tag[1:],
                "tag does not match committed VERSION")
        self.notes_raw = self.file_at_tag(f"release-notes/{self.tag}.md")
        try:
            self.notes = self.notes_raw.decode("utf-8")
        except UnicodeError as exc:
            raise ReleaseError("release notes are not UTF-8") from exc
        require(self.notes.startswith(f"# {self.product} {self.tag}\n") and len(self.notes.strip().splitlines()) > 1,
                "release notes are missing the approved product/version heading or body")
        record_raw = self.file_at_tag(f"release-notes/{self.tag}.json")
        self.record = release_verify.validate_record(parse_json(record_raw), product=self.product,
                                                     tag=self.tag, repository=self.api.repository)
        require(hashlib.sha256(self.notes_raw).hexdigest() == self.record["notes_sha256"],
                "release notes do not match the reviewed record")
        record_path = self.root / "record.json"
        record_path.write_bytes(record_raw)
        candidate_sha = self.record["candidate"]["sha"]
        comparison = self.api.json("GET", f"{self.prefix}/compare/{candidate_sha}...{self.sha}")
        require(comparison.get("status") in {"ahead", "identical"}, "tag is not descended from candidate")
        self.verify_git_difference()
        workflow = self.api.json("GET", f"{self.prefix}/actions/workflows/build_userland.yml")
        require(workflow.get("path") == WORKFLOW and workflow.get("state") == "active",
                "expected build workflow is unavailable")
        self.workflow_id = positive_id(workflow.get("id"), "build workflow id")
        candidate = self.candidate_run(self.record["candidate"])
        reproduction = self.candidate_run(self.record["reproduction"])
        self.phase = "wait-for-tag-build"
        self.tag_run = self.wait_tag_run()
        self.phase = "download-build-artifacts"
        self.tag_provenance()
        candidate_dir, candidate_id = self.artifact(candidate, "candidate", self.record["candidate"]["artifact_id"])
        reproduction_dir, reproduction_id = self.artifact(reproduction, "reproduction",
                                                        self.record["reproduction"]["artifact_id"])
        self.tag_dir, self.tag_artifact_id = self.artifact(self.tag_run, "tag")
        self.phase = "verify-release-payloads"
        report = release_verify.verify_release(record_path, candidate_dir, reproduction_dir, self.tag_dir,
                                               product=self.product, tag=self.tag, tag_sha=self.sha,
                                               tag_tree=self.tree, repository=self.api.repository)
        self.hashes = {name: sha256(self.tag_dir / name) for name in self.names}
        self.check_tag()
        self.phase = "verified"
        return {"verification": report, "repository": self.api.repository, "tag": self.tag,
                "release_branch": branch,
                "tag_sha": self.sha, "tag_object": self.tag_object, "tag_run_id": self.tag_run["id"],
                "tag_run_attempt": self.tag_run["run_attempt"], "tag_artifact_id": self.tag_artifact_id,
                "candidate_artifact_id": candidate_id, "reproduction_artifact_id": reproduction_id,
                "assets_sha256": self.hashes}

    def release(self):
        # List includes authenticated drafts. Match exact tag; no loose release-name lookup.
        releases = self.api.list(f"{self.prefix}/releases")
        matches = [release for release in releases if release.get("tag_name") == self.tag]
        require(len(matches) <= 1, "multiple releases exist for the tag")
        return matches[0] if matches else None

    def check_release(self, release, *, draft=None):
        release_id = positive_id(release.get("id"), "release id")
        require(release.get("tag_name") == self.tag and release.get("name") == self.tag
                and release.get("body") == self.notes and release.get("prerelease") is False,
                "existing release title, body, tag, or prerelease status conflicts")
        require(type(release.get("draft")) is bool, "invalid release draft status")
        if draft is not None:
            require(release["draft"] is draft, "release publication state changed unexpectedly")
        # Created releases use an exact commit. Existing branch-based releases are ambiguous.
        require(release.get("target_commitish") == self.sha, "existing release target commit conflicts")
        return release_id

    def verify_assets(self, release, label, *, allow_missing=False):
        release_id = self.check_release(release)
        assets = self.api.list(f"{self.prefix}/releases/{release_id}/assets")
        names = [asset.get("name") for asset in assets]
        require(len(names) == len(set(names)) and set(names).issubset(self.names),
                "release has duplicate or unexpected assets")
        if not allow_missing:
            require(set(names) == set(self.names), "release is missing expected assets")
        folder = self.root / label
        folder.mkdir()
        for asset in assets:
            asset_id = positive_id(asset.get("id"), "release asset id")
            name = asset["name"]
            require(asset.get("state") == "uploaded" and asset.get("size") == (self.tag_dir / name).stat().st_size,
                    "existing asset is incomplete or has conflicting size")
            self.api.download(f"{self.prefix}/releases/assets/{asset_id}", folder / name)
            require(sha256(folder / name) == self.hashes[name], "release asset bytes conflict with tag build")
        snapshot = self.asset_snapshot(assets)
        require(self.asset_snapshot(self.api.list(f"{self.prefix}/releases/{release_id}/assets")) == snapshot,
                "release assets changed during download verification")
        self.verified_asset_snapshot = snapshot
        return set(self.names) - set(names)

    @staticmethod
    def asset_snapshot(assets):
        # Release asset bytes cannot be updated in-place through GitHub's API;
        # replacing bytes creates a different asset id. Include the full set.
        return sorted((item.get("id"), item.get("name"), item.get("state"), item.get("size"))
                      for item in assets)

    def pre_mutation(self):
        self.check_tag()
        current = self.api.json("GET", f"{self.prefix}/actions/runs/{self.tag_run['id']}")
        self.check_run(current, self.sha, run_id=self.tag_run["id"],
                       attempt=self.tag_run["run_attempt"], tag=True)
        latest = self.api.json("GET", f"{self.prefix}/releases/latest", missing_ok=True)
        if latest is not None and latest.get("tag_name") != self.tag:
            latest_tag = latest.get("tag_name", "")
            require(TAG_RE.fullmatch(latest_tag), "existing Latest has an unexpected version")
            require(tuple(map(int, latest_tag[1:].split("."))) < tuple(map(int, self.tag[1:].split("."))),
                    "refusing to replace a newer Latest release with an older version")

    def publish(self):
        self.phase = "find-or-create-draft"
        self.pre_mutation()
        existing = self.release()
        if existing is None:
            existing = self.api.json("POST", f"{self.prefix}/releases", {
                "tag_name": self.tag, "target_commitish": self.sha, "name": self.tag,
                "body": self.notes, "draft": True, "prerelease": False, "make_latest": "false",
            })
            self.check_release(existing, draft=True)
        release_id = self.check_release(existing)
        self.release_id = release_id
        if existing["draft"] is False:
            # A completed or uncertain previous run is read-only. Never mutate published releases.
            self.phase = "verify-already-published"
            self.verify_assets(existing, "already-published")
            self.check_latest(release_id)
            self.check_tag()
            return {"release_id": release_id, "state": "already-published-verified"}
        missing = self.verify_assets(existing, "existing-draft", allow_missing=True)
        self.phase = "upload-missing-draft-assets"
        for name in sorted(missing):
            self.pre_mutation()
            current = self.api.json("GET", f"{self.prefix}/releases/{release_id}")
            self.check_release(current, draft=True)
            self.api.upload(release_id, self.tag_dir / name)
        draft = self.api.json("GET", f"{self.prefix}/releases/{release_id}")
        self.check_release(draft, draft=True)
        self.phase = "verify-draft-downloads"
        self.verify_assets(draft, "verified-draft")
        self.pre_mutation()
        self.check_release(self.api.json("GET", f"{self.prefix}/releases/{release_id}"), draft=True)
        require(self.asset_snapshot(self.api.list(f"{self.prefix}/releases/{release_id}/assets"))
                == self.verified_asset_snapshot, "verified draft asset inventory changed before publication")
        self.phase = "publish-latest"
        self.publication_attempted = True
        published = self.api.json("PATCH", f"{self.prefix}/releases/{release_id}",
                                  {"draft": False, "make_latest": "true"})
        self.check_release(published, draft=False)
        # Re-read after the mutation, then independently download every published asset.
        published = self.api.json("GET", f"{self.prefix}/releases/{release_id}")
        self.check_release(published, draft=False)
        self.phase = "verify-published-downloads"
        self.verify_assets(published, "verified-published")
        self.check_latest(release_id)
        self.check_tag()
        return {"release_id": release_id, "state": "published-and-redownload-verified"}

    def check_latest(self, release_id):
        latest = self.api.json("GET", f"{self.prefix}/releases/latest")
        require(latest.get("id") == release_id, "release is not Latest; no automatic published-release mutation")
        self.check_release(latest, draft=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("verify", "publish"))
    parser.add_argument("--product", choices=tuple(REPOSITORIES), required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=10800)
    args = parser.parse_args(argv)
    report = {"mode": args.mode, "product": args.product, "status": "blocked"}
    publisher = None
    result = 1
    try:
        event = parse_json(Path(os.environ["GITHUB_EVENT_PATH"]).read_bytes())
        repository, tag, sha = validate_event(os.environ, event, args.product)
        api = GitHub(repository, os.environ.get("GITHUB_TOKEN", ""))
        with tempfile.TemporaryDirectory(prefix="stable-release-") as directory:
            publisher = Publisher(api, args.product, tag, sha, directory, timeout=args.timeout)
            report.update(publisher.prepare())
            if args.mode == "publish":
                report["publication"] = publisher.publish()
            report["status"] = "success"
            print(f"{tag}: {args.mode} completed; report: {args.report}")
        result = 0
    except (ReleaseError, release_verify.VerificationError, KeyError, ValueError, OSError) as exc:
        report["error"] = str(exc)
        print(f"release blocked: {exc}", file=sys.stderr)
        if publisher is not None and publisher.publication_attempted:
            print("Publication was attempted: the release may already be public. Inspect its state; "
                  "reruns verify existing assets and never overwrite or roll back a published release.", file=sys.stderr)
    if publisher is not None:
        report.update({"phase": publisher.phase, "publication_attempted": publisher.publication_attempted,
                       "release_id": publisher.release_id})
    try:
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError:
        print("could not preserve release verification report", file=sys.stderr)
        return 1
    return result


if __name__ == "__main__":
    sys.exit(main())
