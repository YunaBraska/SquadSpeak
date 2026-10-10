"""Exercise the Linux publication job through its real shell entrypoints."""
import json
import os
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
        if mode == "tag": print("refs/tags/2026.10.4")
    elif mode == "release": print("2026.10.4")
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

    def run_step(self, name, workflow="release.yml", **values):
        # Execute the actual workflow shell body, replacing only the external
        # GitHub CLI boundary. Do not duplicate publication logic in the test.
        lines = (self.repo / ".github/workflows" / workflow).read_text(encoding="utf-8").splitlines()
        start = lines.index("      - name: " + name)
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
                   REQUEST_RELEASE="true", FORCE="false", DEFAULT_BRANCH="main",
                   DRY_RUN="false", SNAPSHOT_VERSION="2026.10.4-SNAPSHOT")
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

    def test_merge_pr_and_feature_release_never_publish(self):
        self.repository()
        for values in ({"REQUEST_RELEASE": "false"}, {"SOURCE_REF": "refs/heads/feature/test"},
                       {"SOURCE_REF": "refs/heads/feature/test", "FORCE": "true"},
                       {"SOURCE_REF": "refs/pull/1/merge", "FORCE": "true"}):
            with self.subTest(values=values):
                mode = self.mode(**values)
                self.assertEqual(mode["dry_run"], "true")
                self.assertEqual(mode["commit_sha"], self.git("rev-parse", "HEAD"))
        self.assertFalse((self.root / "trace.jsonl").exists())

    def test_first_release_source_changes_and_explicit_force(self):
        self.repository()
        self.assertEqual(self.mode()["dry_run"], "false")
        self.git("tag", "2026.10.3")
        self.assertEqual(self.mode()["dry_run"], "true")
        self.assertEqual(self.mode(FORCE="true")["dry_run"], "false")
        (self.root / "source.cpp").write_text("changed\n")
        self.git("commit", "--quiet", "-am", "Change source")
        self.assertEqual(self.mode()["dry_run"], "false")

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
        self.assertEqual(self.mode()["dry_run"], "true")
        self.assertEqual(self.mode(FORCE="true")["dry_run"], "false")

    def test_latest_tag_is_reachable_and_sorted_by_creation(self):
        self.repository()
        self.git("tag", "-a", "2098.1.1", "-m", "Older tag")
        self.git("tag", "-a", "2026.10.3", "-m", "Latest tag", date="2026-10-03T13:00:00Z")
        self.git("checkout", "--quiet", "--orphan", "unmerged")
        self.git("commit", "--quiet", "-m", "Unrelated source")
        self.git("tag", "2099.1.1")
        self.git("checkout", "--quiet", "main")
        self.assertEqual(self.mode()["latest_tag"], "2026.10.3")

    def test_deleted_source_is_a_product_change(self):
        self.repository()
        self.git("tag", "2026.10.3")
        self.git("rm", "source.cpp")
        self.git("commit", "--quiet", "-m", "Remove source")
        self.assertEqual(self.mode()["dry_run"], "false")

    def test_default_branch_name_is_not_hardcoded(self):
        self.repository()
        self.assertEqual(self.mode(SOURCE_REF="refs/heads/trunk", DEFAULT_BRANCH="trunk")["dry_run"], "false")
        self.assertEqual(self.mode(SOURCE_REF="refs/heads/main", DEFAULT_BRANCH="trunk")["dry_run"], "true")

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
        result = self.run_step("Resolve build version", "verify.yml", DRY_RUN="true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.outputs()["version"], "2026.10.4-SNAPSHOT")
        self.assertFalse((self.root / "trace.jsonl").exists())
        for invalid in ("", "invalid", "2026.10.4", "1.2.3-SNAPSHOT;bad"):
            (self.root / "output").unlink(missing_ok=True)
            result = self.run_step("Resolve build version", "verify.yml", DRY_RUN="true",
                                   SNAPSHOT_VERSION=invalid)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "output").exists())

    def test_new_version_can_proceed(self):
        result = self.run_step("Resolve build version", "verify.yml")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.outputs()["version"], "2026.10.4")

    def test_existing_release_tag_or_api_failure_stops_before_builds(self):
        for mode in ("release", "tag", "network"):
            with self.subTest(mode=mode):
                result = self.run_step("Resolve build version", "verify.yml", SCENARIO=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "output").exists())

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
