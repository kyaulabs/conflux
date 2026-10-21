"""Release orchestration checks: local synthetic Git history and mocked GitHub."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("conflux_release", REPO / ".github/scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
MERGE, HEAD, BASE = "a" * 40, "b" * 40, "c" * 40
REPOSITORY = "kyaulabs/conflux"


def release_record():
    return {"version": "0.1.0", "sha": MERGE, "head_sha": HEAD, "previous": None, "pr": 7,
            "provenance": {"version": "0.1.0", "develop_sha": BASE, "mixing_evidence": "docs/mixing.md"}}


def pull_request():
    return {"number": 7, "merged": True, "draft": False, "merge_commit_sha": MERGE,
        "head": {"ref": "release/0.1.0", "sha": HEAD, "repo": {"full_name": REPOSITORY}},
        # Deliberately mutable/current base SHA, not the immutable first parent.
        "base": {"ref": "main", "sha": MERGE, "repo": {"full_name": REPOSITORY}}}


def successful_run():
    return {"id": 42, "path": release.WORKFLOW, "status": "completed", "conclusion": "success",
        "event": "pull_request", "head_sha": MERGE, "head_branch": "release/0.1.0",
        "repository": {"full_name": REPOSITORY}, "head_repository": {"full_name": REPOSITORY},
        "pull_requests": [{"number": 7}]}


def jobs():
    return [{"name": name, "status": "completed", "conclusion": "success"} for name in
            ("validate", "publish", "verify / linux", "verify / coverage", "verify / release-tools")]


class ReleaseUnitTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(mock.patch.stopall)
        mock.patch.dict(os.environ, {"GITHUB_REPOSITORY": REPOSITORY, "GITHUB_TOKEN": "fixture-default",
                                    "GH_TOKEN": "fixture-bot"}).start()
        for name in ("run", "git", "api"):
            mock.patch.object(release, name, side_effect=AssertionError("Unexpected external " + name)).start()

    def test_stable_versions_and_global_highest_tag_guard(self):
        self.assertEqual(release.version("0.1.0"), (0, 1, 0))
        for value in (None, "v0.1.0", "01.0.0", "1.2.3-rc1", "1.2", "release/1.2.3"):
            with self.subTest(value=value), self.assertRaises(ValueError): release.version(value)
        with mock.patch.object(release, "stable_tags", return_value=["0.1.0", "2.0.0"]), \
             mock.patch.object(release, "recommend") as recommendation:
            with self.assertRaisesRegex(ValueError, "higher stable"): release.floor("1.0.0", HEAD)
            recommendation.assert_not_called()
        with mock.patch.object(release, "stable_tags", return_value=["0.1.0"]), \
             mock.patch.object(release, "git", return_value=HEAD):
            with self.assertRaisesRegex(ValueError, "another commit"):
                release.floor("0.1.0", BASE, existing_sha=MERGE)

    def test_resolve_exact_verified_merge_head_without_mutable_base_sha_assumption(self):
        def git(*args):
            if args[:3] == ("rev-list", "--parents", "-n"): return f"{MERGE} {BASE} {HEAD}"
            if args[0] == "rev-parse": return "d" * 40
            return ""
        def api(path, **_):
            return pull_request() if path == "pulls/7" else {"sha": MERGE, "commit": {"verification": {"verified": True}}}
        with mock.patch.object(release, "api", side_effect=api), mock.patch.object(release, "git", side_effect=git), \
             mock.patch.object(release, "validate_tree", return_value=(None, release_record()["provenance"])):
            self.assertEqual(release.resolve(7), release_record())
        for parent_line in (f"{MERGE} {HEAD}", f"{MERGE} {BASE} {'e' * 40}", f"{MERGE} {BASE} {HEAD} {'f' * 40}"):
            with self.subTest(parents=parent_line), mock.patch.object(release, "api", side_effect=api), \
                 mock.patch.object(release, "git", side_effect=lambda *a: parent_line if a[0] == "rev-list" else ""):
                with self.assertRaisesRegex(ValueError, "two-parent"): release.resolve(7)

    def test_resolve_rejects_fork_nonrelease_and_unverified_merge(self):
        for change in ("fork", "branch", "unmerged", "unverified"):
            with self.subTest(change=change):
                pr = pull_request()
                if change == "fork": pr["head"]["repo"]["full_name"] = "outside/conflux"
                elif change == "branch": pr["head"]["ref"] = "feat/feature"
                elif change == "unmerged": pr["merged"] = False
                def api(path, **_):
                    return pr if path == "pulls/7" else {"sha": MERGE, "commit": {"verification": {"verified": False}}}
                with mock.patch.object(release, "api", side_effect=api), \
                     mock.patch.object(release, "git", side_effect=lambda *a: f"{MERGE} {BASE} {HEAD}" if a[0] == "rev-list" else ""):
                    with self.assertRaises(ValueError): release.resolve(7)

    def test_publish_missing_token_moving_tag_and_foreign_notes_fail_without_writes(self):
        with mock.patch.dict(os.environ, {"GITHUB_TOKEN": ""}):
            with self.assertRaisesRegex(ValueError, "GITHUB_TOKEN"): release.publish(release_record())
        with mock.patch.object(release, "notes", return_value="approved notes"), \
             mock.patch.object(release, "remote_tag", return_value=HEAD):
            with self.assertRaisesRegex(ValueError, "move"): release.publish(release_record())
        bad = {"tag_name": "v0.1.0", "draft": False, "prerelease": False, "name": "CONFLUX v0.1.0", "body": "foreign notes"}
        with mock.patch.object(release, "notes", return_value="approved notes"), \
             mock.patch.object(release, "remote_tag", return_value=MERGE), \
             mock.patch.object(release, "api", return_value=bad) as api:
            with self.assertRaisesRegex(ValueError, "notes differ"): release.publish(release_record())
            self.assertTrue(all(call.kwargs.get("method", "GET") == "GET" for call in api.call_args_list))

    def test_publish_is_idempotent_and_only_creates_tag_and_release(self):
        published = {"tag_name": "v0.1.0", "draft": False, "prerelease": False,
                     "name": "CONFLUX v0.1.0", "body": "approved notes\r\n"}
        exists = {"tag": False, "release": False}
        def api(path, **kwargs):
            if path == "git/refs":
                self.assertEqual(kwargs, {"method": "POST", "ref": "refs/tags/v0.1.0", "sha": MERGE})
                exists["tag"] = True; return {}
            self.assertEqual(path, "releases/tags/v0.1.0")
            return published if exists["release"] else None
        def command(*args, **kwargs):
            self.assertEqual(args[:4], ("gh", "release", "create", "v0.1.0"))
            self.assertEqual(kwargs["env"]["GH_TOKEN"], "fixture-default")
            exists["release"] = True
            return ""
        with mock.patch.object(release, "notes", return_value="approved notes"), \
             mock.patch.object(release, "remote_tag", side_effect=lambda *_a, **_k: MERGE if exists["tag"] else None), \
             mock.patch.object(release, "api", side_effect=api) as calls, \
             mock.patch.object(release, "run", side_effect=command) as commands:
            release.publish(release_record()); release.publish(release_record())
            self.assertEqual(commands.call_count, 1)
            self.assertEqual(sum(c.args == ("git/refs",) for c in calls.call_args_list), 1)

    def test_failed_api_write_cannot_claim_publication(self):
        with mock.patch.object(release, "notes", return_value="approved notes"), \
             mock.patch.object(release, "remote_tag", return_value=None), \
             mock.patch.object(release, "api", side_effect=subprocess.CalledProcessError(1, ["gh"], stderr="HTTP 403")):
            with self.assertRaisesRegex(ValueError, "Tag creation failed"): release.publish(release_record())

    def run_api(self, workflow, job_rows=None, fallback=None):
        def api(path, **kwargs):
            self.assertEqual(kwargs.get("token"), "GH_TOKEN")
            if path == "actions/runs/42": return workflow
            if path.startswith("actions/runs/42/jobs?"): return {"jobs": jobs() if job_rows is None else job_rows}
            if path.startswith("commits/"): return fallback or []
            if path == "pulls/7": return pull_request()
            raise AssertionError(path)
        return api

    def test_release_run_requires_exact_workflow_repo_event_and_successful_jobs(self):
        with mock.patch.object(release, "api", side_effect=self.run_api(successful_run())), \
             mock.patch.object(release, "resolve", return_value=release_record()) as resolve:
            self.assertEqual(release.release_from_run(42), release_record())
            resolve.assert_called_once_with(7, token="GH_TOKEN")
        for key, bad in (("path", ".github/workflows/build.yml"), ("event", "push"), ("conclusion", "failure")):
            row = successful_run(); row[key] = bad
            with self.subTest(key=key), mock.patch.object(release, "api", side_effect=self.run_api(row)):
                with self.assertRaises(ValueError): release.release_from_run(42)
        for change in ("fork", "tag_dispatch", "skipped_publish", "missing_coverage"):
            row = successful_run(); job_rows = jobs()
            if change == "fork": row["head_repository"]["full_name"] = "outside/conflux"
            elif change == "tag_dispatch": row.update(event="workflow_dispatch", head_branch="v0.1.0")
            elif change == "skipped_publish": job_rows[1]["conclusion"] = "skipped"
            else: job_rows = [j for j in job_rows if j["name"] != "verify / coverage"]
            with self.subTest(change=change), mock.patch.object(release, "api", side_effect=self.run_api(row, job_rows)):
                with self.assertRaises(ValueError): release.release_from_run(42)

    def test_run_fallback_accepts_actual_head_sha_shape_but_not_other_branch_or_latest_main(self):
        row = successful_run(); row["pull_requests"] = []
        with mock.patch.object(release, "api", side_effect=self.run_api(row, fallback=[{"number": 7}])), \
             mock.patch.object(release, "resolve", return_value=release_record()):
            self.assertEqual(release.release_from_run(42)["sha"], MERGE)
        # Real GitHub closed-PR runs can have no pull_requests links and retain
        # the source head SHA rather than merge SHA. Association plus exact
        # release branch/head and strict resolve remain mandatory.
        row["head_sha"] = HEAD
        with mock.patch.object(release, "api", side_effect=self.run_api(row, fallback=[{"number": 7}])), \
             mock.patch.object(release, "resolve", return_value=release_record()):
            self.assertEqual(release.release_from_run(42)["sha"], MERGE)
        row["head_branch"] = "unrelated"
        with mock.patch.object(release, "api", side_effect=self.run_api(row, fallback=[{"number": 7}])):
            with self.assertRaisesRegex(ValueError, "exact merged"): release.release_from_run(42)

    def test_backmerge_bot_current_main_and_one_matching_pr_no_commits(self):
        record = release_record(); marker = f"<!-- conflux-backmerge:v0.1.0 sha={MERGE} -->"
        pr = {"number": 12, "state": "open", "body": marker, "user": {"login": "kyaulabs-bot"},
              "head": {"ref": "main", "sha": MERGE, "repo": {"full_name": REPOSITORY}},
              "base": {"ref": "develop", "repo": {"full_name": REPOSITORY}}}
        with mock.patch.object(release, "run", return_value='{"login":"kyaulabs-bot"}'), \
             mock.patch.object(release, "release_from_run", return_value=record), \
             mock.patch.object(release, "stable_publication"), mock.patch.object(release, "git", return_value=MERGE) as git, \
             mock.patch.object(release, "backmerge_body", return_value=("body " + marker, marker)), \
             mock.patch.object(release, "pages", return_value=[pr]):
            self.assertEqual(release.backmerge(42), 12)
            git.assert_called_once_with("rev-parse", "origin/main")
        for change in ("wrong_bot", "moved_main", "foreign_pr", "duplicate"):
            current = copy.deepcopy(pr)
            if change == "foreign_pr": current["user"]["login"] = "other"
            with self.subTest(change=change), \
                 mock.patch.object(release, "run", return_value=json.dumps({"login": "kyau" if change == "wrong_bot" else "kyaulabs-bot"})), \
                 mock.patch.object(release, "release_from_run", return_value=record), \
                 mock.patch.object(release, "stable_publication"), \
                 mock.patch.object(release, "git", return_value=HEAD if change == "moved_main" else MERGE), \
                 mock.patch.object(release, "backmerge_body", return_value=("body", marker)), \
                 mock.patch.object(release, "pages", return_value=[current, current] if change == "duplicate" else [current]):
                with self.assertRaises(ValueError): release.backmerge(42)
        with mock.patch.dict(os.environ, {"GH_TOKEN": ""}):
            with self.assertRaisesRegex(ValueError, "GH_TOKEN"): release.backmerge(42)

    def test_backmerge_create_uses_main_develop_only_and_leaves_ci_incomplete(self):
        for moved in (False, True):
            response = {"number": 12, "state": "open", "body": "marker\n- [ ] Backmerge CI",
                "user": {"login": "kyaulabs-bot"},
                "head": {"ref": "main", "sha": HEAD if moved else MERGE, "repo": {"full_name": REPOSITORY}},
                "base": {"ref": "develop", "repo": {"full_name": REPOSITORY}}}
            with self.subTest(main_advanced_after_check=moved), \
                 mock.patch.object(release, "run", return_value='{"login":"kyaulabs-bot"}'), \
                 mock.patch.object(release, "release_from_run", return_value=release_record()), \
                 mock.patch.object(release, "stable_publication"), mock.patch.object(release, "git", return_value=MERGE), \
                 mock.patch.object(release, "backmerge_body", return_value=("marker\n- [ ] Backmerge CI", "marker")), \
                 mock.patch.object(release, "pages", return_value=[]), \
                 mock.patch.object(release, "api", return_value=response) as api:
                if moved:
                    with self.assertRaisesRegex(ValueError, "leave any created PR"): release.backmerge(42)
                else:
                    self.assertEqual(release.backmerge(42), 12)
                api.assert_called_once()  # no automatic close, retry or second PR
                self.assertEqual(api.call_args.args, ("pulls",))
                self.assertEqual(api.call_args.kwargs["head"], "main")
                self.assertEqual(api.call_args.kwargs["base"], "develop")
                self.assertIn("- [ ]", api.call_args.kwargs["body"])

    def test_template_unknown_placeholder_and_nonrelease_main_pr_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unresolved"): release.render("{{UNKNOWN}}", {"VERSION": "0.1.0"})
        event = {"pull_request": pull_request()}; event["pull_request"]["head"]["ref"] = "feat/normal"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "event.json"; path.write_text(json.dumps(event))
            with mock.patch.dict(os.environ, {"GITHUB_EVENT_PATH": str(path)}):
                with self.assertRaisesRegex(ValueError, "release/X.Y.Z"): release.check_pr()


@unittest.skipUnless(shutil.which("git") and shutil.which("git-cliff"), "git/git-cliff not installed")
class ReleaseGitCliffTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name)
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(release, "ROOT", self.path).start()
        mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_DATE": "2026-01-02T12:00:00+00:00", "GIT_COMMITTER_DATE": "2026-01-02T12:00:00+00:00",
            "GITHUB_REPOSITORY": REPOSITORY}).start()
        self.g("init", "-b", "develop")
        self.g("config", "user.name", "Synthetic release fixture")
        self.g("config", "user.email", "fixture@example.invalid")
        (self.path / ".github").mkdir(); (self.path / "docs").mkdir()
        (self.path / "cliff.toml").write_bytes((REPO / "cliff.toml").read_bytes())
        (self.path / "CMakeLists.txt").write_text("project(CONFLUX VERSION 0.0.0 LANGUAGES CXX)\n")
        (self.path / "CHANGELOG.md").write_text("# Changelog\n")
        (self.path / "docs/mixing.md").write_text(release.MIXING_MARKER + "\nSynthetic fixture acceptance; not actual product acceptance.\n")
        (self.path / ".github/RELEASE_TEMPLATE.md").write_text("{{VERSION}} {{PREVIOUS}}\n{{CHANGES}}\n{{COMPARE}}\n{{VERIFICATION}}\n")
        self.commit("feat: initial fixture")
        self.base = self.g("rev-parse", "HEAD")
        self.g("branch", "main"); self.g("remote", "add", "origin", str(self.path))
        self.g("fetch", "origin", "main", "develop", "--tags")

    def g(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.path, text=True, stderr=subprocess.PIPE).strip()

    def commit(self, message):
        self.g("add", "."); self.g("commit", "-m", message)

    def test_real_initial_prepare_then_tagged_merge_rerun_keeps_floor(self):
        self.assertEqual(release.recommend(self.base), "0.1.0")
        release.prepare("0.1.0", "docs/mixing.md")
        self.assertEqual(self.g("branch", "--show-current"), "release/0.1.0")
        self.assertEqual(self.g("rev-parse", "HEAD"), self.base)  # no automatic commit
        self.assertEqual(release.project_version((self.path / "CMakeLists.txt").read_text()), "0.1.0")
        self.assertEqual(json.loads((self.path / ".github/release.json").read_text())["develop_sha"], self.base)
        self.commit("chore(release): prepare 0.1.0")
        head = self.g("rev-parse", "HEAD")
        self.g("switch", "main"); self.g("merge", "--no-ff", "release/0.1.0", "-m", "chore(release): merge 0.1.0")
        merge = self.g("rev-parse", "HEAD")
        record = release_record(); record.update(sha=merge, head_sha=head)
        before_tag = release.notes(record)
        self.g("tag", "v0.1.0")
        self.assertEqual(release.notes(record), before_tag)
        pr = pull_request(); pr["head"]["sha"] = head; pr["merge_commit_sha"] = merge; pr["base"]["sha"] = merge
        def api(path, **_):
            return pr if path == "pulls/7" else {"sha": merge, "commit": {"verification": {"verified": True}}}
        with mock.patch.object(release, "api", side_effect=api):
            for _ in range(2):
                self.assertEqual(release.resolve(7)["sha"], merge)

    def test_real_cliff_feature_patch_breaking_and_nonincrement_types(self):
        self.g("tag", "v0.1.0")
        cases = [(kind + ": fixture", None) for kind in ("docs", "ci", "test", "style", "chore", "refactor")]
        cases += [("feat: next feature", "0.2.0"), ("fix: correction", "0.1.1"),
                  ("docs!: incompatible interface", "1.0.0"), ("docs: update\n\nBREAKING CHANGE: incompatible format", "1.0.0")]
        for index, (message, expected) in enumerate(cases):
            with self.subTest(message=message):
                self.g("switch", "-c", "case-" + str(index), self.base)
                (self.path / "change.txt").write_text(str(index)); self.commit(message)
                self.assertEqual(release.recommend(self.g("rev-parse", "HEAD"), "0.1.0"), expected)

    def test_real_merge_subjects_are_skipped_and_evidence_is_not_a_filename_claim(self):
        self.g("tag", "v0.1.0")
        for index in range(2):
            self.g("switch", "-c", "doc-" + str(index), "develop")
            (self.path / ("doc-" + str(index))).write_text("fixture"); self.commit("docs: explanation")
            self.g("switch", "develop"); self.g("merge", "--no-ff", "doc-" + str(index), "-m", "feat: misleading merge title")
        self.assertIsNone(release.recommend(self.g("rev-parse", "HEAD"), "0.1.0"))
        for path in ("../docs/mixing.md", "/docs/mixing.md", "CMakeLists.txt", "docs/missing.md"):
            with self.subTest(path=path), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                release.evidence(self.base, path)
        (self.path / "docs/mixing.md").write_text("No actual acceptance marker.\n"); self.commit("docs: remove fixture marker")
        with self.assertRaisesRegex(ValueError, "Actual mixing acceptance"):
            release.evidence(self.g("rev-parse", "HEAD"), "docs/mixing.md")
        (self.path / "docs/mixing.md").write_text("Use `" + release.MIXING_MARKER + "` only after acceptance.\n")
        self.commit("docs: explain marker syntax")
        with self.assertRaisesRegex(ValueError, "Actual mixing acceptance"):
            release.evidence(self.g("rev-parse", "HEAD"), "docs/mixing.md")


if __name__ == "__main__":
    unittest.main()
