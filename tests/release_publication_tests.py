"""Exercise the Linux publication job through its real shell entrypoints."""
import json
import os
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
    pathlib.Path("public").write_text("complete", encoding="utf-8")
else: sys.exit(7)
''', encoding="utf-8")
        gh.chmod(0o700)

    def run_step(self, name, **values):
        # Execute the actual workflow shell body, replacing only the external
        # GitHub CLI boundary. Do not duplicate publication logic in the test.
        lines = (self.repo / ".github/workflows/release.yml").read_text(encoding="utf-8").splitlines()
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
                   RELEASE_VERSION="2026.10.4", SOURCE_REVISION="a" * 40)
        env.update(values)
        return subprocess.run(["sh", "-eu", "-c", "\n".join(script)], cwd=self.root, env=env,
                              encoding="utf-8", capture_output=True)

    def test_new_version_can_proceed(self):
        result = self.run_step("Resolve UTC release version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "output").read_text(encoding="utf-8"), "value=2026.10.4\n")

    def test_existing_release_tag_or_api_failure_stops_before_builds(self):
        for mode in ("release", "tag", "network"):
            with self.subTest(mode=mode):
                result = self.run_step("Resolve UTC release version", SCENARIO=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "output").exists())
        result = self.run_step("Resolve UTC release version", SOURCE_REF="refs/heads/feature/test")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Release must run from main", result.stderr)

    def test_publish_only_after_all_uploads_succeed(self):
        assets = self.root / "release-assets"
        assets.mkdir()
        (assets / "package.zip").write_bytes(b"package fixture")
        result = self.run_step("Publish the tested desktop release", SCENARIO="upload-failure")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "public").exists())
        trace = self.root / "trace.jsonl"
        self.assertEqual(len(trace.read_text(encoding="utf-8").splitlines()), 1)
        trace.unlink()
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
