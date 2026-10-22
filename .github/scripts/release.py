#!/usr/bin/env python3
"""Prepare releases, publish verified merges, and separately open a bot backmerge PR."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
MIXING_MARKER = "<!-- conflux-mixing-accepted -->"
M2_MARKER = "<!-- conflux-m2-complete -->"
M3_MARKER = "<!-- conflux-m3-complete -->"
WORKFLOW = ".github/workflows/release.yml"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def run(*args, env=None, input=None):
    return subprocess.run(args, cwd=ROOT, env=env, input=input, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout.strip()


def git(*args):
    return run("git", *args)


def version(value):
    require(type(value) is str and VERSION.fullmatch(value), "Expected stable X.Y.Z version")
    return tuple(map(int, value.split(".")))


def repo():
    value = os.environ.get("GITHUB_REPOSITORY", "")
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value), "GITHUB_REPOSITORY is required")
    return value


def auth(token):
    require(os.environ.get(token), token + " is required")
    return {**os.environ, "GH_TOKEN": os.environ[token]}


def api(path, *, method="GET", token="GITHUB_TOKEN", optional=False, **fields):
    args = ["gh", "api", "--method", method, f"repos/{repo()}/{path}"]
    if fields:
        args += ["--input", "-"]
    try:
        value = run(*args, env=auth(token), input=json.dumps(fields) if fields else None)
    except subprocess.CalledProcessError as error:
        if optional and "HTTP 404" in (error.stderr or ""):
            return None
        raise
    return json.loads(value or "null")


def pages(path, *, token="GITHUB_TOKEN", key=None):
    rows = []
    for page in range(1, 101):
        batch = api(path + ("&" if "?" in path else "?") + f"per_page=100&page={page}", token=token)
        batch = batch.get(key) if key and type(batch) is dict else batch
        require(type(batch) is list, "Malformed paginated GitHub response")
        rows.extend(batch)
        if len(batch) < 100:
            return rows
    raise ValueError("Incomplete GitHub pagination")


def stable_tags():
    return [tag[1:] for tag in git("tag", "--list", "v*").splitlines() if VERSION.fullmatch(tag[1:])]


def cliff(sha, previous=None, *options):
    require(SHA.fullmatch(sha), "Invalid changelog commit")
    require(run("git-cliff", "--version") == "git-cliff 2.14.2", "git-cliff 2.14.2 is required")
    span = f"v{previous}..{sha}" if previous else sha
    # Use the committed configuration for the requested tree, not an unrelated
    # checkout. Merge subjects must not become duplicate release entries/bumps.
    with tempfile.NamedTemporaryFile(mode="w", suffix=".toml") as config:
        config.write(git("show", f"{sha}:cliff.toml")); config.flush()
        args = ["git-cliff", "--offline", "--no-exec", "--use-branch-tags", "--config", config.name,
                "--tag-pattern", r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"]
        for merge in git("rev-list", "--merges", span).splitlines():
            args += ["--skip-commit", merge]
        return run(*args, *options, span)


def recommend(sha, previous=None):
    if previous:
        git("merge-base", "--is-ancestor", "v" + previous, sha)
    value = cliff(sha, previous, "--unreleased", "--bumped-version").removeprefix("v")
    if not value:
        return None
    version(value)
    return None if previous and version(value) <= version(previous) else value


def floor(target, base, *, existing_sha=None):
    version(target)
    tags = stable_tags()
    require(not tags or version(target) >= max(map(version, tags)), "A higher stable release already exists")
    if target in tags:
        require(existing_sha is not None and git("rev-parse", f"v{target}^{{commit}}") == existing_sha,
                "Release tag already exists at another commit")
    previous = max((x for x in tags if version(x) < version(target)), key=version, default=None)
    required = recommend(base, previous)
    require(required is not None and version(target) >= version(required),
            "Version is below git-cliff recommendation or there are no releasable changes")
    require(previous is not None or target == "0.1.0", "The first release must be 0.1.0")
    return previous


def evidence(tree, path, target):
    require(type(path) is str and path.endswith(".md") and not path.startswith("/")
            and str(PurePosixPath(path)) == path and ".." not in PurePosixPath(path).parts,
            "Mixing evidence must be a relative tracked Markdown file")
    entry = git("ls-tree", tree, "--", path).split()
    require(len(entry) == 4 and entry[0] in ("100644", "100755") and entry[1] == "blob"
            and entry[3] == path, "Mixing evidence must be a tracked regular file")
    text = git("show", f"{tree}:{path}")
    markers = (MIXING_MARKER, M2_MARKER, M3_MARKER) if target == "0.1.0" else (MIXING_MARKER,)
    for marker in markers:
        require(re.search(r"(?m)^" + re.escape(marker) + r"\r?$", text),
                "Actual mixing acceptance must be recorded before release" if marker == MIXING_MARKER
                else "First 0.1.0 release requires recorded " + ("M2" if marker == M2_MARKER else "M3") + " completion")
    for marker in markers:
        text = text.replace(marker, "")
    require(text.strip(), "Acceptance evidence requires nonempty prose")
    return git("rev-parse", f"{tree}:{path}")


def project_version(text):
    matches = re.findall(r"(?m)^project\(CONFLUX VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$", text)
    require(len(matches) == 1, "Expected one CONFLUX CMake project version")
    return matches[0]


def validate_tree(head, target, *, existing_sha=None):
    require(SHA.fullmatch(head), "Invalid release head")
    provenance = json.loads(git("show", f"{head}:.github/release.json"))
    require(set(provenance) == {"version", "develop_sha", "mixing_evidence"}
            and provenance["version"] == target and SHA.fullmatch(provenance["develop_sha"]),
            "Release provenance is missing or invalid")
    base = provenance["develop_sha"]
    git("merge-base", "--is-ancestor", base, "origin/develop")
    git("merge-base", "--is-ancestor", base, head)
    require(project_version(git("show", f"{head}:CMakeLists.txt")) == target, "CMake version differs")
    require(evidence(head, provenance["mixing_evidence"], target) == evidence(base, provenance["mixing_evidence"], target),
            "Mixing evidence differs from recorded develop source")
    changelog = git("show", f"{head}:CHANGELOG.md")
    require(re.search(r"(?m)^##\s+.*(?<![0-9.])v?" + re.escape(target) + r"(?=[\]\s(])", changelog),
            "Release version is absent from changelog heading")
    paths = set(git("diff", "--name-only", base, head).splitlines())
    require(paths <= {"CMakeLists.txt", "CHANGELOG.md", ".github/release.json"},
            "Release branch must contain only preparation changes after recorded develop")
    previous = floor(target, base, existing_sha=existing_sha)
    return previous, provenance


def resolve(number, *, token="GITHUB_TOKEN"):
    require(str(number).isdigit() and int(number) > 0, "Release PR number is required")
    pr = api(f"pulls/{int(number)}", token=token)
    require(pr.get("merged") is True and pr.get("draft") is False and pr["base"]["ref"] == "main"
            and pr["head"]["repo"]["full_name"] == pr["base"]["repo"]["full_name"] == repo(),
            "Expected a merged same-repository release PR into main")
    branch = pr["head"]["ref"]
    require(branch.startswith("release/"), "Only release/X.Y.Z may publish")
    target = branch.removeprefix("release/"); version(target)
    sha, head = pr["merge_commit_sha"], pr["head"]["sha"]
    require(SHA.fullmatch(sha) and SHA.fullmatch(head), "Invalid release commit identity")
    git("fetch", "origin", "main", "develop", "--tags")
    git("merge-base", "--is-ancestor", sha, "origin/main")
    parents = git("rev-list", "--parents", "-n", "1", sha).split()
    require(len(parents) == 3 and parents[0] == sha and parents[2] == head,
            "Release must be a two-parent merge with the exact PR head")
    commit = api(f"commits/{sha}", token=token)
    require(commit.get("sha") == sha and commit["commit"]["verification"].get("verified") is True,
            "Release merge is not verified")
    previous, provenance = validate_tree(head, target, existing_sha=sha)
    for path in ("CMakeLists.txt", "CHANGELOG.md", ".github/release.json", provenance["mixing_evidence"]):
        require(git("rev-parse", f"{sha}:{path}") == git("rev-parse", f"{head}:{path}"),
                "Merged release provenance/version/changelog/evidence differs")
    return {"version": target, "sha": sha, "head_sha": head, "pr": int(number),
            "previous": previous, "provenance": provenance}


def prepare(target, mixing_evidence):
    version(target)
    require(not git("status", "--porcelain"), "Commit existing changes before release preparation")
    git("fetch", "origin", "main", "develop", "--tags")
    base = git("rev-parse", "origin/develop")
    floor(target, base)
    evidence(base, mixing_evidence, target)
    cmake = git("show", f"{base}:CMakeLists.txt")
    old = project_version(cmake)
    changelog = cliff(base, None, "--tag", "v" + target)
    require(changelog.strip(), "git-cliff returned an empty changelog")
    git("switch", "-c", "release/" + target, base)
    (ROOT / "CMakeLists.txt").write_text(cmake.replace("VERSION " + old + " LANGUAGES", "VERSION " + target + " LANGUAGES", 1) + "\n")
    (ROOT / "CHANGELOG.md").write_text(changelog + "\n")
    (ROOT / ".github/release.json").write_text(json.dumps({"version": target, "develop_sha": base,
        "mixing_evidence": mixing_evidence}, indent=2) + "\n")
    print(f"Prepared release/{target}; review, sign and commit the three preparation files. No commit or push performed.")


def render(template, values):
    for key, value in values.items():
        template = template.replace("{{" + key + "}}", value)
    require(not re.search(r"\{\{[^{}]+\}\}", template), "Unresolved release template placeholder")
    return template


def notes(release):
    target, previous, sha = release["version"], release["previous"], release["sha"]
    tag = "v" + target
    compare = f"https://github.com/{repo()}/compare/v{previous}...{tag}" if previous else f"https://github.com/{repo()}/commits/{tag}"
    return render(git("show", f"{sha}:.github/RELEASE_TEMPLATE.md"), {
        "REPOSITORY": repo(), "TAG": tag, "VERSION": target, "PREVIOUS": "v" + previous if previous else "Initial release",
        "CHANGES": cliff(sha, previous, "--tag", tag, "--strip", "all"), "COMPARE": "[Full comparison](" + compare + ")",
        "VERIFICATION": "Verified release merge, CMake version, changelog, semantic-version floor and "
                        + "[recorded mixing acceptance](https://github.com/" + repo() + "/blob/" + tag + "/"
                        + quote(release["provenance"]["mixing_evidence"], safe="/")
                        + "). Build/test results remain in the release workflow; backmerge CI is separate."})


def remote_tag(tag, *, token):
    row = api("git/ref/tags/" + tag, token=token, optional=True)
    if row is None:
        return None
    obj = row["object"]
    for _ in range(4):
        if obj["type"] == "commit":
            require(SHA.fullmatch(obj["sha"]), "Invalid remote tag commit")
            return obj["sha"]
        require(obj["type"] == "tag" and SHA.fullmatch(obj["sha"]), "Unsupported remote tag object")
        obj = api("git/tags/" + obj["sha"], token=token)["object"]
    raise ValueError("Nested tag bound exceeded")


def stable_publication(release, *, token, expected_body=None):
    tag = "v" + release["version"]
    require(remote_tag(tag, token=token) == release["sha"], "Published tag differs from verified merge")
    published = api("releases/tags/" + tag, token=token, optional=True)
    require(published is not None and published.get("tag_name") == tag and published.get("draft") is False
            and published.get("prerelease") is False, "Stable published release is missing")
    expected_body = notes(release) if expected_body is None else expected_body
    normalize = lambda text: text.replace("\r\n", "\n").rstrip("\n")
    require(published.get("name") == "CONFLUX " + tag and type(published.get("body")) is str
            and normalize(published["body"]) == normalize(expected_body), "Existing release title/notes differ; no overwrite")
    return published


def publish(release):
    auth("GITHUB_TOKEN")
    tag, sha = "v" + release["version"], release["sha"]
    body = notes(release)  # Fail template/rendering checks before external writes.
    current = remote_tag(tag, token="GITHUB_TOKEN")
    require(current in (None, sha), "Refusing to move an existing tag")
    if current is None:
        try:
            api("git/refs", method="POST", ref="refs/tags/" + tag, sha=sha)
        except subprocess.CalledProcessError:
            require(remote_tag(tag, token="GITHUB_TOKEN") == sha, "Tag creation failed or raced another commit")
    existing = api("releases/tags/" + tag, optional=True)
    if existing is None:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md") as out:
            out.write(body); out.flush()
            try:
                run("gh", "release", "create", tag, "--repo", repo(), "--verify-tag", "--target", sha,
                    "--title", "CONFLUX " + tag, "--notes-file", out.name, env=auth("GITHUB_TOKEN"))
            except subprocess.CalledProcessError:
                stable_publication(release, token="GITHUB_TOKEN", expected_body=body)
    stable_publication(release, token="GITHUB_TOKEN", expected_body=body)


def release_from_run(run_id):
    require(str(run_id).isdigit() and int(run_id) > 0, "Release workflow run ID is required")
    workflow = api(f"actions/runs/{int(run_id)}", token="GH_TOKEN")
    require(workflow.get("id") == int(run_id) and workflow.get("path") == WORKFLOW
            and workflow.get("status") == "completed" and workflow.get("conclusion") == "success"
            and workflow.get("event") in ("pull_request", "workflow_dispatch")
            and workflow["repository"]["full_name"] == repo()
            and workflow["head_repository"]["full_name"] == repo(), "Wrong/unsuccessful release workflow run")
    head = workflow.get("head_sha", ""); require(SHA.fullmatch(head), "Missing upstream workflow SHA")
    require(workflow["event"] != "workflow_dispatch" or workflow.get("head_branch") == "main",
            "Manual release retry must originate from main, never a tag")
    jobs = pages(f"actions/runs/{int(run_id)}/jobs?filter=latest", token="GH_TOKEN", key="jobs")
    for name in ("validate", "publish", "verify / linux", "verify / coverage", "verify / release-tools"):
        selected = [job for job in jobs if job.get("name") == name]
        require(len(selected) == 1 and selected[0].get("status") == "completed"
                and selected[0].get("conclusion") == "success", "Required release job did not succeed: " + name)
    links = workflow.get("pull_requests", [])
    require(type(links) is list, "Invalid upstream pull request links")
    candidates = links or pages(f"commits/{head}/pulls", token="GH_TOKEN")
    matches = []
    for candidate in candidates:
        pr = api(f"pulls/{int(candidate['number'])}", token="GH_TOKEN")
        if pr.get("merged") is True and pr.get("merge_commit_sha") == head:
            matches.append(pr["number"])
        elif (workflow["event"] == "pull_request" and pr.get("merged") is True
              and pr["head"]["sha"] == head and workflow.get("head_branch") == pr["head"]["ref"]):
            matches.append(pr["number"])
    require(len(set(matches)) == 1, "Run lacks one exact merged release PR; no latest-main fallback")
    release = resolve(matches[0], token="GH_TOKEN")
    require((workflow["event"] == "pull_request"
             and workflow.get("head_branch") == "release/" + release["version"]
             and head in (release["sha"], release["head_sha"]))
            or (workflow["event"] == "workflow_dispatch" and head == release["sha"]),
            "Upstream run must identify the exact release head/branch or main merge, never latest main")
    return release


def backmerge_body(release, run_id):
    tag, sha = "v" + release["version"], release["sha"]
    marker = f"<!-- conflux-backmerge:{tag} sha={sha} -->"
    text = re.sub(r"<!--.*?-->", "", git("show", f"{sha}:.github/PULL_REQUEST_TEMPLATE.md"), flags=re.S).strip()
    sections = {
        "## 📋 Summary": f"Bring released {tag} from main into develop.\n\n{marker}",
        "## 📦 Changes by Phase": "- Preserve the released version and merge ancestry; no new authored commits.",
        "## 📜 ADRs": "None.",
        "## ✅ Verification": f"Release workflow {run_id} completed successfully for {sha}. Backmerge CI remains required.",
        "## 🏗️ Architect Conditions (if applicable)": "Not applicable.",
        "## 📝 Commits (<# total>)": "No new commits. Head is the verified published release merge.",
        "## 🧪 Test Plan": "- [x] Release workflow completed successfully.\n- [x] Stable tag, provenance and current main SHA verified.\n- [ ] Backmerge PR CI passed against develop.",
    }
    for heading, content in sections.items():
        require(heading in text, "Required PR template section missing: " + heading)
        text = text.replace(heading, heading + "\n\n" + content)
    return text.replace("## 📝 Commits (<# total>)", "## 📝 Commits") + "\n", marker


def backmerge(run_id):
    who = json.loads(run("gh", "api", "user", env=auth("GH_TOKEN")))
    require(who.get("login") == "kyaulabs-bot", "Backmerge GH_TOKEN must belong to kyaulabs-bot")
    release = release_from_run(run_id)
    stable_publication(release, token="GH_TOKEN")
    require(git("rev-parse", "origin/main") == release["sha"], "Main moved beyond this release; refusing stale backmerge")
    body, marker = backmerge_body(release, run_id)
    candidates = pages(f"pulls?state=all&base=develop&head={repo().split('/')[0]}:main", token="GH_TOKEN")
    matching = [p for p in candidates if marker in (p.get("body") or "") or p.get("state") == "open"]
    require(len(matching) <= 1, "Ambiguous/existing main-to-develop pull requests")
    if matching:
        pr = matching[0]
    else:
        pr = api("pulls", method="POST", token="GH_TOKEN", title=f"chore(release): backmerge v{release['version']} into develop",
                 head="main", base="develop", body=body)
    require(pr["user"]["login"] == "kyaulabs-bot" and pr["head"]["ref"] == "main"
            and pr["base"]["ref"] == "develop" and pr["head"]["sha"] == release["sha"]
            and pr["head"]["repo"]["full_name"] == pr["base"]["repo"]["full_name"] == repo()
            and marker in (pr.get("body") or "")
            and (pr.get("state") == "open" or (matching and pr.get("merged_at"))),
            "Backmerge response provenance differs; leave any created PR for review")
    return pr["number"]


def check_pr():
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    pr = event["pull_request"]
    if pr["base"]["ref"] != "main":
        return
    require(pr["head"]["repo"]["full_name"] == pr["base"]["repo"]["full_name"] == repo()
            and pr["head"]["ref"].startswith("release/"), "Only same-repository release/X.Y.Z PRs may target main")
    target = pr["head"]["ref"].removeprefix("release/"); version(target)
    require(git("rev-parse", "HEAD") == pr["head"]["sha"], "Check the actual PR head source, not synthetic merge source")
    git("fetch", "origin", "main", "develop", "--tags")
    validate_tree(pr["head"]["sha"], target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("recommend", "prepare", "check-pr", "validate", "publish", "backmerge"))
    parser.add_argument("value", nargs="?")
    parser.add_argument("--mixing-evidence")
    args = parser.parse_args()
    if args.action == "prepare":
        return prepare(args.value, args.mixing_evidence)
    if args.action == "check-pr":
        return check_pr()
    if args.action == "recommend":
        tags = stable_tags(); previous = max(tags, key=version, default=None)
        print(recommend(git("rev-parse", args.value or "origin/develop"), previous) or "")
        return
    if args.action == "backmerge":
        result = {"backmerge": backmerge(args.value or os.environ.get("WORKFLOW_RUN_ID", ""))}
    else:
        result = resolve(args.value or os.environ.get("RELEASE_PR", ""))
        if args.action == "publish":
            publish(result)
        result = {key: result[key] for key in ("sha", "version")}
    if os.environ.get("GITHUB_OUTPUT"):
        with Path(os.environ["GITHUB_OUTPUT"]).open("a") as out:
            for key, value in result.items():
                out.write(f"{key}={value}\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
