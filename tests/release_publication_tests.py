"""Exercise the Linux publication job through its real shell entrypoints."""
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class PublicationTests(unittest.TestCase):
    assets = ("squadspeak-linux-aarch64.tar.gz", "squadspeak-linux-x86_64.tar.gz",
              "squadspeak-macos-arm64.zip", "squadspeak-macos-x86_64.zip",
              "squadspeak-windows-x86_64.zip", "squadspeak-windows-device-test.zip",
              "squadspeak-third-party-sources.tar.gz", "appcast-macos-arm64.xml", "appcast-macos-x86_64.xml")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = Path(__file__).resolve().parents[1]
        (self.root / "cmake").symlink_to(self.repo / "cmake", target_is_directory=True)
        (self.root / "bin").mkdir()
        gh = self.root / "bin/gh"
        gh.write_text("#!" + sys.executable + "\n" + '''import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ["TRACE"], "a", encoding="utf-8") as trace:
    trace.write(json.dumps(args) + "\\n")
mode = os.environ.get("SCENARIO", "")
if args[0] == "api":
    if mode == "network": sys.exit(4)
    if "matching-refs" in " ".join(args):
        if mode == "tag": print("refs/tags/" + os.environ.get("COLLISION_VERSION", "2026.10.4"))
    elif mode == "release": print(os.environ.get("COLLISION_VERSION", "2026.10.4"))
elif args[:2] == ["release", "create"]:
    if mode == "upload-failure": sys.exit(5)
    pathlib.Path("uploaded").write_text("complete", encoding="utf-8")
elif args[:2] == ["release", "edit"]:
    if not pathlib.Path("uploaded").exists(): sys.exit(6)
    if mode == "promotion-failure": sys.exit(8)
    pathlib.Path("public").write_text("complete", encoding="utf-8")
else: sys.exit(7)
''', encoding="utf-8")
        gh.chmod(0o700)

    def run_step(self, name, workflow="publish.yml", **values):
        # Execute the actual workflow shell body, replacing only the external
        # GitHub CLI boundary. Do not duplicate publication logic in the test.
        lines = (self.repo / ".github/workflows" / workflow).read_text(encoding="utf-8").splitlines()
        matches = [index for index, line in enumerate(lines)
                   if re.fullmatch(r'      - name: "?[^\n]*' + re.escape(name) + r'"?', line)]
        self.assertEqual(len(matches), 1, f"Missing or ambiguous workflow step: {name}")
        start = matches[0]
        run = next(i for i in range(start, len(lines)) if lines[i] == "        run: |") + 1
        script = []
        for line in lines[run:]:
            if line and not line.startswith("          "):
                break
            script.append(line[10:])
        env = dict(os.environ, PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                   TRACE=str(self.root / "trace.jsonl"), RUNNER_TEMP=str(self.root),
                   GITHUB_OUTPUT=str(self.root / "output"), SOURCE_DATE_EPOCH="1791072000",
                   GH_REPO="owner/project", GH_TOKEN="test-token", SOURCE_REF="refs/heads/main",
                   RELEASE_VERSION="2026.10.4", SOURCE_REVISION="a" * 40,
                   REQUEST_MODE="release", FORCE="false", DEFAULT_BRANCH="main",
                   RELEASE_MODE="release", SNAPSHOT_VERSION="2026.10.4-SNAPSHOT", GITHUB_RUN_ID="17")
        env.update(values)
        return subprocess.run(["sh", "-eu", "-c", "\n".join(script)], cwd=self.root, env=env,
                              encoding="utf-8", capture_output=True)

    def git(self, *args, date="2026-10-03T12:00:00Z"):
        return subprocess.check_output(["git", *args], cwd=self.root, text=True,
            env=dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date,
                     GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")).strip()

    def repository(self):
        self.git("init", "--quiet", "--initial-branch=main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release@example.invalid")
        (self.root / "source.cpp").write_text("original\n")
        self.git("add", "source.cpp")
        self.git("commit", "--quiet", "-m", "Initial source")

    def outputs(self):
        return dict(line.split("=", 1) for line in
                    (self.root / "output").read_text(encoding="utf-8").splitlines())

    def mode(self, **values):
        (self.root / "output").unlink(missing_ok=True)
        result = self.run_step("Read build mode", "verify.yml", **values)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.outputs()

    def test_build_pr_and_feature_runs_never_publish(self):
        self.repository()
        for values in ({"REQUEST_MODE": "build"}, {"SOURCE_REF": "refs/heads/feature/test"},
                       {"SOURCE_REF": "refs/heads/feature/test", "FORCE": "true"},
                       {"SOURCE_REF": "refs/heads/feature/test", "REQUEST_MODE": "snapshot"},
                       {"SOURCE_REF": "refs/pull/1/merge", "FORCE": "true"}):
            with self.subTest(values=values):
                mode = self.mode(**values)
                self.assertEqual(mode["release_mode"], "build")
                self.assertEqual(mode["commit_sha"], self.git("rev-parse", "HEAD"))
        self.assertFalse((self.root / "trace.jsonl").exists())

    def test_default_branch_snapshot_is_publishable_without_force(self):
        self.repository()
        self.git("tag", "2026.10.3")
        self.assertEqual(self.mode(REQUEST_MODE="snapshot")["release_mode"], "snapshot")
        self.assertEqual(self.mode(REQUEST_MODE="snapshot", DEFAULT_BRANCH="trunk",
                                  SOURCE_REF="refs/heads/trunk")["release_mode"], "snapshot")

    def test_unknown_release_mode_fails_before_publication(self):
        self.repository()
        result = self.run_step("Read build mode", "verify.yml", REQUEST_MODE="unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown release mode", result.stderr)
        self.assertFalse((self.root / "output").exists())
        self.assertFalse((self.root / "trace.jsonl").exists())

    def test_first_release_source_changes_and_explicit_force(self):
        self.repository()
        self.assertEqual(self.mode()["release_mode"], "release")
        self.git("tag", "2026.10.3")
        self.assertEqual(self.mode()["release_mode"], "build")
        self.assertEqual(self.mode(FORCE="true")["release_mode"], "release")
        (self.root / "source.cpp").write_text("changed\n")
        self.git("commit", "--quiet", "-am", "Change source")
        self.assertEqual(self.mode()["release_mode"], "release")

    def test_documentation_tests_and_workflows_require_force(self):
        self.repository()
        self.git("tag", "2026.10.3")
        for name in ("README.md", "docs/topic.txt", "tests/case.cpp", "specs/channel.txt",
                     "examples/sample.cpp", ".github/workflows/build.yml", "LICENSE", ".gitignore"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("changed\n")
            self.git("add", name)
        self.git("commit", "--quiet", "-m", "Non-product changes")
        self.assertEqual(self.mode()["release_mode"], "build")
        self.assertEqual(self.mode(FORCE="true")["release_mode"], "release")

    def test_latest_tag_is_reachable_and_sorted_by_creation(self):
        self.repository()
        self.git("tag", "-a", "2098.1.1", "-m", "Older tag")
        self.git("tag", "-a", "2026.10.3", "-m", "Latest tag", date="2026-10-03T13:00:00Z")
        self.git("checkout", "--quiet", "--orphan", "unmerged")
        self.git("commit", "--quiet", "-m", "Unrelated source")
        self.git("tag", "2099.1.1")
        self.git("checkout", "--quiet", "main")
        self.assertEqual(self.mode()["latest_tag"], "2026.10.3")

    def test_snapshot_tag_does_not_hide_changes_since_stable_release(self):
        self.repository()
        self.git("tag", "2026.10.3")
        (self.root / "source.cpp").write_text("changed\n")
        self.git("commit", "--quiet", "-am", "Change source", date="2026-10-04T12:00:00Z")
        self.git("tag", "2026.10.4-SNAPSHOT.17")
        mode = self.mode()
        self.assertEqual(mode["latest_tag"], "2026.10.3")
        self.assertEqual(mode["release_mode"], "release")

    def test_snapshot_version_distinguishes_workflow_runs(self):
        for run_id in ("17", "18"):
            with self.subTest(run_id=run_id):
                result = self.run_step("Resolve build version", "verify.yml", RELEASE_MODE="build",
                                       GITHUB_RUN_ID=run_id)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.outputs()["version"], f"2026.10.4-SNAPSHOT.{run_id}")
        for run_id in ("", "0", "01", "17-SNAPSHOT", "17;bad"):
            with self.subTest(invalid_run=run_id):
                (self.root / "output").unlink(missing_ok=True)
                result = self.run_step("Resolve build version", "verify.yml", RELEASE_MODE="build",
                                       GITHUB_RUN_ID=run_id)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "output").exists())

    def test_deleted_source_is_a_product_change(self):
        self.repository()
        self.git("tag", "2026.10.3")
        self.git("rm", "source.cpp")
        self.git("commit", "--quiet", "-m", "Remove source")
        self.assertEqual(self.mode()["release_mode"], "release")

    def test_default_branch_name_is_not_hardcoded(self):
        self.repository()
        self.assertEqual(self.mode(SOURCE_REF="refs/heads/trunk", DEFAULT_BRANCH="trunk")["release_mode"], "release")
        self.assertEqual(self.mode(SOURCE_REF="refs/heads/main", DEFAULT_BRANCH="trunk")["release_mode"], "build")

    def test_git_failure_stops_resolution(self):
        result = self.run_step("Read build mode", "verify.yml")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "output").exists())

    def test_failed_tag_lookup_does_not_look_like_an_initial_release(self):
        self.repository()
        git = self.root / "bin/git"
        git.write_text("#!" + sys.executable + "\nimport os, sys\n"
                       "if sys.argv[1] == 'tag': sys.exit(9)\n"
                       f"os.execv({shutil.which('git')!r}, ['git'] + sys.argv[1:])\n")
        git.chmod(0o700)
        result = self.run_step("Read build mode", "verify.yml")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "output").exists())

    def test_snapshot_uses_action_version_without_github_mutations(self):
        result = self.run_step("Resolve build version", "verify.yml", RELEASE_MODE="build")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.outputs()["version"], "2026.10.4-SNAPSHOT.17")
        self.assertFalse((self.root / "trace.jsonl").exists())
        for invalid in ("", "invalid", "2026.10.4", "1.2.3-SNAPSHOT;bad"):
            (self.root / "output").unlink(missing_ok=True)
            result = self.run_step("Resolve build version", "verify.yml", RELEASE_MODE="build",
                                   SNAPSHOT_VERSION=invalid)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "output").exists())

    def test_new_version_can_proceed(self):
        result = self.run_step("Resolve build version", "verify.yml")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.outputs()["version"], "2026.10.4")

    def test_existing_release_tag_or_api_failure_stops_before_builds(self):
        for release_mode, version in (("release", "2026.10.4"), ("snapshot", "2026.10.4-SNAPSHOT.17")):
            for scenario in ("release", "tag", "network"):
                with self.subTest(mode=release_mode, failure=scenario):
                    result = self.run_step("Resolve build version", "verify.yml", SCENARIO=scenario,
                                           RELEASE_MODE=release_mode, COLLISION_VERSION=version)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((self.root / "output").exists())

    def test_new_snapshot_checks_collisions_and_resolves_version(self):
        result = self.run_step("Resolve build version", "verify.yml", RELEASE_MODE="snapshot")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.outputs()["version"], "2026.10.4-SNAPSHOT.17")
        calls = [json.loads(line) for line in (self.root / "trace.jsonl").read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(call[0] == "api" for call in calls))

    def test_update_keys_fail_before_native_builds(self):
        import base64
        seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        public = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        private_key = base64.b64encode(seed).decode()
        public_key = base64.b64encode(public).decode()
        for mode in ("build", "snapshot"):
            result = self.run_step("Validate update signing", "verify.yml", RELEASE_MODE=mode,
                                   UPDATE_PRIVATE_KEY="", UPDATE_PUBLIC_KEY="")
            self.assertEqual(result.returncode, 0, result.stderr)
        for private, pub, succeeds in (("", "", False), (private_key, "", False),
                ("invalid", public_key, False), (private_key, base64.b64encode(bytes(32)).decode(), False),
                (private_key, public_key, True), (private_key + "\n", public_key, True)):
            with self.subTest(valid=succeeds):
                result = self.run_step("Validate update signing", "verify.yml", RELEASE_MODE="release",
                                       UPDATE_PRIVATE_KEY=private, UPDATE_PUBLIC_KEY=pub)
                self.assertEqual(result.returncode == 0, succeeds, result.stderr)
                self.assertNotIn(private_key, result.stdout + result.stderr)
                self.assertFalse((self.root / "update-key.der").exists())

    def test_publish_only_after_all_uploads_succeed(self):
        assets = self.root / "release-assets"
        assets.mkdir()
        (assets / "package.zip").write_bytes(b"package fixture")
        trace = self.root / "trace.jsonl"
        for mode, calls in (("upload-failure", 1), ("promotion-failure", 2)):
            with self.subTest(mode=mode):
                result = self.run_step("Publish the tested desktop release", SCENARIO=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "public").exists())
                self.assertEqual((self.root / "uploaded").exists(), mode == "promotion-failure")
                self.assertEqual(len(trace.read_text(encoding="utf-8").splitlines()), calls)
                trace.unlink()
                (self.root / "uploaded").unlink(missing_ok=True)
        result = self.run_step("Publish the tested desktop release")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        self.assertIn("--draft", calls[0])
        self.assertIn("--target", calls[0])
        self.assertIn("a" * 40, calls[0])
        self.assertIn("--draft=false", calls[1])
        self.assertIn("--prerelease=false", calls[1])
        self.assertIn("--latest=true", calls[1])
        self.assertTrue((self.root / "public").exists())

    def test_snapshot_publication_never_updates_latest(self):
        assets = self.root / "release-assets"
        assets.mkdir()
        (assets / "package.zip").write_bytes(b"package fixture")
        result = self.run_step("Publish the tested desktop release", RELEASE_MODE="snapshot",
                               RELEASE_VERSION="2026.10.4-SNAPSHOT.17")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in (self.root / "trace.jsonl").read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertIn("--draft", calls[0])
        self.assertIn("a" * 40, calls[0])
        self.assertIn("--prerelease=true", calls[1])
        self.assertTrue(all("--latest=false" in call for call in calls))
        self.assertTrue((self.root / "public").exists())
        notes = (self.root / "release-notes.md").read_text()
        self.assertIn("Snapshot preview", notes)
        self.assertNotIn("squadspeak.rb is", notes)

    def test_snapshot_package_set_omits_update_feeds_and_homebrew(self):
        assets = self.root / "release-assets"
        assets.mkdir()
        expected = self.assets[:-2]
        for name in expected:
            (assets / name).write_bytes(name.encode())
        values = dict(RELEASE_MODE="snapshot", RELEASE_VERSION="2026.10.4-SNAPSHOT.17")
        for name in ("appcast-macos-arm64.xml", "squadspeak.rb"):
            (assets / name).write_text("unexpected")
            result = self.run_step("Verify package set and create checksums", **values)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((assets / "SHA256SUMS").exists())
            (assets / name).unlink()
        result = self.run_step("Verify package set and create checksums", **values)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((assets / "squadspeak.rb").exists())
        checksums = (assets / "SHA256SUMS").read_text()
        self.assertEqual(len(checksums.splitlines()), len(expected))
        for name in expected:
            self.assertIn("  " + name + "\n", checksums)

    def test_publication_mode_must_match_version(self):
        (self.root / "release-assets").mkdir()
        for mode, version in (("snapshot", "2026.10.4"), ("release", "2026.10.4-SNAPSHOT.17"),
                              ("unknown", "2026.10.4")):
            with self.subTest(mode=mode):
                result = self.run_step("Verify package set and create checksums",
                                       RELEASE_MODE=mode, RELEASE_VERSION=version)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "release-assets/SHA256SUMS").exists())

    def test_package_set_rejects_missing_empty_and_unexpected_files(self):
        assets = self.root / "release-assets"
        assets.mkdir()
        for name in self.assets:
            (assets / name).write_bytes(name.encode())
        target = assets / self.assets[0]
        for invalid in ("missing", "empty", "unexpected"):
            with self.subTest(invalid=invalid):
                if invalid == "missing": target.unlink()
                elif invalid == "empty": target.write_bytes(b"")
                else: (assets / "unexpected.txt").write_bytes(b"wrong artifact")
                result = self.run_step("Verify package set and create checksums")
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((assets / "SHA256SUMS").exists())
                target.write_bytes(self.assets[0].encode())
        (assets / "unexpected.txt").unlink()
        result = self.run_step("Verify package set and create checksums")
        self.assertEqual(result.returncode, 0, result.stderr)
        checksums = (assets / "SHA256SUMS").read_text(encoding="utf-8")
        self.assertEqual(len(checksums.splitlines()), len(self.assets) + 1)
        for name in (*self.assets, "squadspeak.rb"):
            self.assertIn("  " + name + "\n", checksums)


if __name__ == "__main__":
    unittest.main()
