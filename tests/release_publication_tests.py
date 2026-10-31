"""Exercise build and publication shell entrypoints at their CLI boundaries."""
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

    def test_only_intel_ci_requests_delivery_paced_audio(self):
        workflow = (self.repo / ".github/workflows/verify.yml").read_text()
        command = re.search(r"^  macos:\n.*?^      test_command: \|\n((?:        [^\n]*\n)+)",
                            workflow, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(command)
        script = "\n".join(line[8:] for line in command[1].splitlines())
        (self.root / "tests").mkdir()
        (self.root / "tests/macos-ci.sh").write_text(
            "#!/bin/sh\nexec \"$TEST_PYTHON\" -c 'import json, os; "
            "print(json.dumps({k: v for k, v in os.environ.items() if k.startswith(\"SQUAD_SOAK_\")}))'\n")
        for architecture in ("X64", "ARM64"):
            with self.subTest(architecture=architecture):
                env = {key: value for key, value in os.environ.items() if not key.startswith("SQUAD_SOAK_")}
                env.update(RUNNER_ARCH=architecture, TEST_PYTHON=sys.executable)
                result = subprocess.run(["sh", "-eu", "-c", script], cwd=self.root, env=env,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = {"SQUAD_SOAK_CLIENTS": "32", "SQUAD_SOAK_SPEAKERS": "2",
                            "SQUAD_SOAK_PACING": "delivery"} if architecture == "X64" else {}
                self.assertEqual(json.loads(result.stdout), expected)
        # The hosted network launcher clears unlisted environment variables.
        preserved = re.search(r"--preserve-env=([^\s]+)",
                              (self.repo / "CMakeLists.txt").read_text())[1].split(",")
        self.assertIn("SQUAD_SOAK_PACING", preserved)

    def test_network_launcher_keeps_function_inventory_and_resource_guards(self):
        executable = self.root / "network contract"
        executable.write_text("#!" + sys.executable + "\n" + '''import sys
if sys.argv[1] == "-functions":
    print("boundedDiscoveryAcceptsKnownHosts()")
    print("sixtyFourClientsHaveOneConsistentRosterAndAudioFanout()")
    print("repeatedDiscoveryDoesNotRebuildUnchangedChannels()")
    print("discoveryRefreshesLivenessWithoutRepeatingVisibleUpdates()")
    print("unchangedDiscoveryHeartbeatDoesNotRebuildChannels()")
    print("addChannelCandidatesExcludeSavedHostsAndDuplicateEndpoints()")
else:
    print("EXECUTED " + sys.argv[1])
''')
        executable.chmod(0o700)
        launcher = self.root / "bin/launcher"
        launcher.write_text("#!/bin/sh\n[ \"$1\" = \"argument with spaces\" ] || exit 6\nshift\nexec \"$@\"\n")
        launcher.chmod(0o700)
        # macOS sudo is executable but not readable to the test account.
        execute_only = self.root / "bin/execute-only-env"
        source = self.root / "launcher.c"
        source.write_text("#include <unistd.h>\nint main(int argc, char **argv) {\n"
                          "    return argc > 1 ? execvp(argv[1], argv + 1) : 2;\n}\n")
        compiled = subprocess.run(["cc", str(source), "-o", str(execute_only)],
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        execute_only.chmod(0o111)
        discovered = self.root / "discovered.cmake"
        for prefix in ("", str(launcher) + ";argument with spaces", str(execute_only)):
            result = subprocess.run(["cmake", "-DCHANNEL_TEST_EXECUTABLE=" + str(executable),
                "-DOUTPUT_FILE=" + str(discovered), "-DTEST_LAUNCHER=" + prefix,
                "-P", str(self.repo / "cmake/DiscoverChannelTests.cmake")], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            (self.root / "CTestTestfile.cmake").write_text(discovered.read_text())
            result = subprocess.run(["ctest", "--test-dir", str(self.root), "--show-only=json-v1"],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            tests = json.loads(result.stdout)["tests"]
            self.assertEqual(len(tests), 6)
            properties = [{p["name"]: p["value"] for p in test["properties"]} for test in tests]
            self.assertEqual(properties[0]["RESOURCE_LOCK"], ["channel_fixed_ports"])
            self.assertTrue(properties[1]["RUN_SERIAL"])
            for discovery in properties[2:]:
                self.assertEqual(discovery.get("RESOURCE_LOCK"), ["channel_fixed_ports"])
            result = subprocess.run(["ctest", "--test-dir", str(self.root), "--parallel", "2", "-V"],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("EXECUTED "), 6)

    def test_ci_qml_selectors_exist_before_native_builds(self):
        available = set()
        for path in (self.repo / "tests").glob("tst_*.qml"):
            source = path.read_text()
            suite = re.search(r'^\s*name:\s*"(\w+)"', source, re.MULTILINE)
            if suite:
                available.update(suite[1] + "::" + name for name in
                                 re.findall(r"\bfunction\s+(test_\w+)\s*\(", source))
        scripts = list((self.repo / "tests").glob("*-ci.*")) + list((self.repo / ".github/workflows").glob("*.yml"))
        selected = {(path.name, name) for path in scripts for name in
                    re.findall(r"\b\w+::test_\w+", path.read_text())}
        self.assertTrue(selected, "No explicit QML test selections found")
        self.assertEqual(sorted((path, name) for path, name in selected if name not in available), [])

    def test_native_rust_tests_are_prebuilt_outside_the_test_deadline(self):
        (self.root / "src").mkdir()
        (self.root / "src/teamspeak_bridge").symlink_to(self.repo / "src/teamspeak_bridge", target_is_directory=True)
        (self.root / "rust-toolchain.toml").symlink_to(self.repo / "rust-toolchain.toml")
        cargo = self.root / "bin/cargo"
        cargo.write_text("#!" + sys.executable + "\n" + '''import os, pathlib, sys
root = pathlib.Path(os.environ["CARGO_TARGET_DIR"])
if "OPENSSL_DIR" in os.environ and not os.environ["OPENSSL_DIR"]:
    sys.exit("An empty OpenSSL override hides the system installation")
if os.environ.get("PROTOC") != sys.executable:
    sys.exit("The configured protobuf compiler was not passed to Cargo")
if os.environ.get("OPENSSL_DIR") and os.environ.get("OPENSSL_STATIC") != "1":
    sys.exit("The packaged OpenSSL library must be linked statically")
root.mkdir(parents=True, exist_ok=True)
args = sys.argv[1:]
with (root / "calls").open("a") as log: log.write(" ".join(args) + "\\n")
if args[0] == "build":
    (root / "release").mkdir(exist_ok=True)
    (root / "release/libsquadspeak_teamspeak_bridge.a").touch()
elif args[0] == "test" and "--no-run" in args:
    if os.environ.get("FAIL_PREPARE"): sys.exit(37)
    (root / "tests-ready").touch()
elif args[0] == "test":
    if not (root / "tests-ready").is_file():
        sys.exit("Test attempted to compile dependencies inside its deadline")
else: sys.exit(38)
''')
        cargo.chmod(0o700)
        (self.root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(BuildContract NONE)
include(CTest)
include(cmake/TeamSpeak.cmake)
add_custom_target(consumer ALL DEPENDS squad_teamspeak_build)
''')
        for mode, flags in (("native", []), ("production", ["-DBUILD_TESTING=OFF"]),
                            ("cross", ["-DCMAKE_SYSTEM_NAME=Generic"]),
                            ("pinned", ["-DBUILD_TESTING=OFF", "-DOPENSSL_ROOT_DIR=" + str(self.root / "packaged-openssl")])):
            with self.subTest(mode=mode):
                build = self.root / mode
                result = subprocess.run(["cmake", "-S", str(self.root), "-B", str(build),
                    "-DSQUAD_CARGO=" + str(cargo), "-DSQUAD_RUSTC=" + sys.executable,
                    "-DSQUAD_PROTOC=" + sys.executable, *flags],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(["cmake", "--build", str(build), "--parallel", "2"],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if mode != "native":
                    self.assertNotIn("test ", (build / "teamspeak/calls").read_text())
                    continue
                result = subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error"],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(["cmake", "--build", str(build)],
                    env=dict(os.environ, FAIL_PREPARE="1"), capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, "Test preparation failure must fail the build")

    def test_dependency_cache_ignores_test_and_release_changes(self):
        (self.root / "cmake").unlink()
        shutil.copytree(self.repo / "cmake", self.root / "cmake")
        (self.root / "tests").mkdir()
        for name in ("CMakeLists.txt", "tests/macos-ci.sh", "tests/windows-ci.ps1", "tests/linux-ci.sh"):
            shutil.copy2(self.repo / name, self.root / name)
        for name in ("xcodebuild", "xcrun"):
            tool = self.root / "bin" / name
            tool.write_text("#!/bin/sh\nprintf 'fixed toolchain\\n'\n")
            tool.chmod(0o700)
        workflow = (self.repo / ".github/workflows/verify.yml").read_text()
        selections = re.findall(r"^      cache_inputs: '(.*)'$", workflow, re.MULTILINE)
        self.assertEqual(len(selections), 3)
        for selection in selections:
            def fingerprint():
                linux = "tests/linux-ci.sh" in json.loads(selection)
                result = self.run_step("Read Dependency Inputs" if linux else "Read Toolchain",
                                       "wc_linux_build.yml" if linux else "wc_macos_build.yml", CACHE_INPUTS=selection)
                self.assertEqual(result.returncode, 0, result.stderr)
                return self.outputs()["inputs"]
            baseline = fingerprint()
            for name in ("CMakeLists.txt", "cmake/Version.cmake", "cmake/DiscoverChannelTests.cmake"):
                with self.subTest(selection=selection, unchanged=name):
                    path = self.root / name
                    original = path.read_text()
                    path.write_text(original + "\n# unrelated test or release change\n")
                    try:
                        self.assertEqual(fingerprint(), baseline)
                    finally:
                        path.write_text(original)
            for name in ("cmake/qt-network-security.patch", "cmake/qt-wayland.patch"):
                with self.subTest(selection=selection, linux_only=name):
                    path = self.root / name
                    original = path.read_text()
                    path.write_text(original + "\n# changed Linux-only repair\n")
                    try:
                        self.assertEqual(fingerprint() != baseline, "tests/linux-ci.sh" in json.loads(selection))
                    finally:
                        path.write_text(original)
            pins = ["cmake/Dependencies.cmake", "cmake/AudioProcessing.cmake", "cmake/Media.cmake",
                    "cmake/Updates.cmake", "cmake/qt-qml-runtime.patch", "cmake/source_archives.json"]
            pins += [name for name in json.loads(selection) if name.startswith("tests/")]
            for name in pins:
                with self.subTest(selection=selection, changed=name):
                    path = self.root / name
                    original = path.read_text()
                    path.write_text(original + "\n# changed dependency input\n")
                    try:
                        self.assertNotEqual(fingerprint(), baseline)
                    finally:
                        path.write_text(original)

    def test_macos_schedules_all_contracts_together_and_preserves_failures(self):
        for name, script in (("uname", "#!/bin/sh\nprintf 'arm64\\n'\n"),
                             ("sudo", '#!/bin/sh\nshift\nexec "$@"\n')):
            path = self.root / "bin" / name
            path.write_text(script)
            path.chmod(0o700)
        ctest = self.root / "bin/ctest"
        ctest.write_text("#!" + sys.executable + "\n" + '''import json, os, pathlib, sys
root = pathlib.Path(os.environ["TEST_BUILD"])
with open(root / "trace.jsonl", "a") as log:
    log.write(json.dumps({"args": sys.argv[1:], "version": os.environ.get("SQUADSPEAK_VERSION")}) + "\\n")
reports = root / "artifacts"
(reports / "network.xml").write_text("failure report")
(reports / pathlib.Path(sys.argv[sys.argv.index("--output-log") + 1]).name).write_text("network failed")
details = root / "app/Testing/Temporary/LastTest.log"
details.parent.mkdir(parents=True, exist_ok=True)
details.write_text("failed test details")
sys.exit(9)
''')
        ctest.chmod(0o700)
        for version, native, expected in (("", "", None), ("2026.10.11-SNAPSHOT.17", "", "2026.10.11-SNAPSHOT.17"),
                                          ("2026.10.11", "2026.10.10", "2026.10.10")):
            with self.subTest(version=version, native=native):
                (self.root / "trace.jsonl").unlink(missing_ok=True)
                result = subprocess.run(["sh", str(self.repo / "tests/macos-ci.sh"), str(self.root),
                                         str(self.root), "test"], capture_output=True, text=True, timeout=20,
                    env=dict(os.environ, PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                             GITHUB_ACTIONS="true", TEST_BUILD=str(self.root), VERSION=version, SQUADSPEAK_VERSION=native))
                self.assertNotEqual(result.returncode, 0)
                calls = [json.loads(line) for line in (self.root / "trace.jsonl").read_text().splitlines()]
                self.assertEqual(len(calls), 1, result.stderr)
                self.assertNotIn("-R", calls[0]["args"])
                self.assertNotIn("-E", calls[0]["args"])
                self.assertIn("--parallel", calls[0]["args"])
                self.assertNotIn("--stop-on-failure", calls[0]["args"])
                self.assertEqual(calls[0]["version"], expected)
                self.assertEqual((self.root / "artifacts/test-details.log").read_text(), "failed test details")
                self.assertEqual((self.root / "artifacts/ctest.log").read_text(), "network failed")

    def test_discovery_uses_the_image_built_by_the_linux_workflow(self):
        workflow = (self.repo / ".github/workflows/verify.yml").read_text()
        image = re.search(r"^\s+image: (\S+)$", workflow, re.MULTILINE).group(1)
        artifacts = self.root / "artifacts"
        artifacts.mkdir()
        probe = artifacts / "discovery_network_probe"
        probe.write_text("#!/bin/sh\nexit 0\n")
        probe.chmod(0o700)
        docker = self.root / "bin/docker"
        # Only replace Docker. The real network-change script must use the
        # image prepared by its caller before any probe can start.
        docker.write_text("#!" + sys.executable + "\n" + '''import os, pathlib, sys
args = sys.argv[1:]
if args[0] == "run":
    image = args[args.index("--entrypoint") + 2]
    if image != os.environ["BUILT_IMAGE"]:
        sys.exit("Network probe requested an image the workflow did not build: " + image)
    evidence = next(pathlib.Path(os.environ["TEST_ARTIFACTS"]).glob("network-change.*"))
    for name in ("receiver.ready", "sender-1.ready", "sender-2.ready", "sender-1.passed", "sender-2.passed"):
        (evidence / name).touch()
elif args[0] == "wait": print(0)
''')
        docker.chmod(0o700)
        result = subprocess.run(["sh", str(self.repo / "tests/discovery-network.sh"), str(artifacts)],
            env=dict(os.environ, PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                     BUILT_IMAGE=image, TEST_ARTIFACTS=str(artifacts)), capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_macos_package_signs_nested_helpers_and_requires_accepted_notarization(self):
        boundary = "#!" + sys.executable + "\n" + '''import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["TRACE"], "a") as output:
    output.write(json.dumps([name] + args) + "\\n")
scenario = os.environ["SCENARIO"]
if name == "uname": print("arm64")
elif name == "cmake":
    app = pathlib.Path(args[args.index("--prefix") + 1]) / "squadspeak.app"
    (app / "Contents/Resources").mkdir(parents=True)
    framework = app / "Contents/Frameworks/Sparkle.framework/Versions/B"
    for path in ("XPCServices/Installer.xpc", "XPCServices/Downloader.xpc", "Updater.app"):
        (framework / path).mkdir(parents=True)
    (framework / "Autoupdate").touch()
elif name == "codesign" and scenario == "signing-failure": sys.exit(9)
elif name == "ditto":
    destination = pathlib.Path(args[-1])
    if "-x" in args: destination.mkdir(parents=True, exist_ok=True)
    else: destination.write_bytes(b"archive")
elif name == "python3":
    if not args[0].endswith("check_macos_bundle.py"):
        os.execv(sys.executable, [sys.executable] + args)
elif name == "xcrun" and args[:2] == ["notarytool", "submit"]:
    if scenario == "submit-failure": sys.exit(7)
    if scenario == "malformed": print("not json")
    else: print(json.dumps({"id": "submission-1", "status": "Accepted" if scenario == "log-failure" else scenario}))
elif name == "xcrun" and args[:2] == ["notarytool", "log"]:
    if scenario == "log-failure": sys.exit(8)
    pathlib.Path(args[-1]).write_text(json.dumps({"status": scenario, "issues": []}))
'''
        for name in ("uname", "cmake", "codesign", "macdeployqt", "ditto", "python3", "xcrun"):
            path = self.root / "bin" / name
            path.write_text(boundary)
            path.chmod(0o700)
        for scenario in ("Accepted", "Invalid", "In Progress", "malformed", "submit-failure",
                         "log-failure", "signing-failure"):
            with self.subTest(scenario=scenario):
                trace = self.root / "trace.jsonl"
                trace.unlink(missing_ok=True)
                build = self.root / scenario
                runtime = build / "app/ffmpeg/lib"
                runtime.mkdir(parents=True)
                for library in ("avcodec.61", "avutil.59", "swscale.8", "avformat.61", "swresample.5"):
                    (runtime / f"lib{library}.dylib").write_bytes(b"patched runtime")
                result = subprocess.run(["sh", str(self.repo / "tests/macos-ci.sh"),
                    str(self.root), str(build), "package"], capture_output=True, text=True, timeout=20,
                    env=dict(os.environ, PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"],
                        TRACE=str(trace), SCENARIO=scenario, TMPDIR=str(self.root), RUNNER_TEMP=str(self.root),
                        MACOS_SIGNING_IDENTITY="Release Test", MACOS_NOTARY_PROFILE="test-profile", SIGN_UPDATES="false"))
                calls = [json.loads(line) for line in trace.read_text().splitlines()]
                stapling = [call[1:3] for call in calls if call[0] == "xcrun" and call[1] == "stapler"]
                if scenario == "Accepted":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(stapling, [["stapler", "staple"], ["stapler", "validate"]])
                    signed = [call for call in calls if call[0] == "codesign" and "--sign" in call]
                    self.assertEqual(len(signed), 10)
                    nested_helpers = signed[:4]
                    self.assertEqual({Path(call[-1]).name for call in nested_helpers},
                                     {"Installer.xpc", "Downloader.xpc", "Autoupdate", "Updater.app"})
                    for call in nested_helpers:
                        self.assertEqual(call[call.index("--sign") + 1], "Release Test")
                        self.assertIn("--timestamp", call)
                        self.assertEqual(call[call.index("--options") + 1], "runtime")
                    downloader = next(call for call in nested_helpers if call[-1].endswith("Downloader.xpc"))
                    self.assertIn("--preserve-metadata=entitlements", downloader)
                    deploy = next(i for i, call in enumerate(calls) if call[0] == "macdeployqt")
                    self.assertTrue(all(calls.index(call) < deploy for call in nested_helpers))
                    runtime_signatures = signed[4:]
                    self.assertEqual({Path(call[-1]).name for call in runtime_signatures},
                                     {"libavcodec.61.dylib", "libavutil.59.dylib", "libswscale.8.dylib",
                                      "libavformat.61.dylib", "libswresample.5.dylib", "squadspeak.app"})
                    self.assertTrue(all(calls.index(call) > deploy for call in runtime_signatures))
                    log = next(i for i, call in enumerate(calls) if call[1:3] == ["notarytool", "log"])
                    staple = next(i for i, call in enumerate(calls) if call[1:3] == ["stapler", "staple"])
                    self.assertLess(log, staple)
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertEqual(stapling, [])
                if scenario in ("Accepted", "Invalid", "In Progress"):
                    self.assertEqual(json.loads((build / "artifacts/notarization-log.json").read_text())["status"], scenario)
                self.assertEqual(list(self.root.glob("squadspeak-package.*")), [])

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
