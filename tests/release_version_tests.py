"""Check release metadata and native compilation commands."""
import argparse
import hashlib
import json
import os
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ReleaseVersionTests(unittest.TestCase):
    def test_apple_bundle_identifier_matches_the_registered_store_identifier(self):
        source = Path(__file__).resolve().parents[1]
        app_store = (source / ".github/workflows/app-store.yml").read_text(encoding="utf-8")
        app_store_upload = (source / ".github/workflows/app-store-upload.yml").read_text(encoding="utf-8")
        bundle = (source / "ui/CMakeLists.txt").read_text(encoding="utf-8")
        appcast = (source / "cmake/build_appcasts.py").read_text(encoding="utf-8")
        self.assertIn("bundle_id: berlin.yuna.squadspeak", app_store)
        self.assertIn("version: ${{ inputs.version }}", app_store)
        self.assertIn("version: ${{ inputs.version }}", app_store_upload)
        self.assertRegex(app_store, r"wc_ios_app_store\.yml@[0-9a-f]{40}")
        self.assertIn("artifact_extension: ipa", app_store_upload)
        self.assertIn('set(SQUADSPEAK_BUNDLE_ID "app.squadspeak.desktop"', bundle)
        self.assertIn('MACOSX_BUNDLE_GUI_IDENTIFIER "${SQUADSPEAK_BUNDLE_ID}"', bundle)
        self.assertIn('SQUADSPEAK_BUNDLE_ID="$APP_STORE_BUNDLE_ID"',
                      (source / "scripts/package-app-store.sh").read_text(encoding="utf-8"))
        self.assertIn('-DSQUADSPEAK_BUNDLE_ID="$APP_STORE_BUNDLE_ID"',
                      (source / "scripts/package-ios-app-store.sh").read_text(encoding="utf-8"))
        self.assertIn('("CFBundleIdentifier", "app.squadspeak.desktop")', appcast)

    def test_ios_device_dependencies_cannot_reuse_the_simulator_prefix(self):
        script = (Path(__file__).resolve().parents[1] / "cmake/build-mobile-dependencies.sh").read_text(encoding="utf-8")
        self.assertIn("ios-simulator|ios-device|android", script)
        self.assertIn("sdk=${IOS_SDK:-iphoneos}", script)
        self.assertIn("marker=\"$prefix/.squadspeak-ios-device-dependencies\"", script)
        self.assertIn("apple_platform=IOS", script)
        self.assertIn("deployment_flag=-miphoneos-version-min", script)
        self.assertIn("marker=\"$prefix/.squadspeak-ios-simulator-dependencies\"", script)

    def test_ios_store_package_keeps_public_and_apple_build_versions_separate(self):
        script = (Path(__file__).resolve().parents[1] / "scripts/package-ios-app-store.sh").read_text(encoding="utf-8")
        for required in (
            'Expected marketing version and build number.',
            'SQUADSPEAK_VERSION="$1"',
            'SQUADSPEAK_APPLE_BUILD_NUMBER="$2"',
            'CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Manual',
            'CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=YES',
            'CMAKE_XCODE_ATTRIBUTE_PROVISIONING_PROFILE="$IOS_APP_STORE_PROFILE_UUID"',
            'Payload/SquadSpeak.app',
            'dist/app-store/SquadSpeak.ipa',
            'codesign --verify --deep --strict',
        ):
            with self.subTest(required=required):
                self.assertIn(required, script)

    def resolve(self, value=None, epoch="1791072000"):
        command = ["cmake"]
        if value is not None:
            command.append("-DSQUADSPEAK_VERSION=" + value)
        command.extend(["-P", str(Path(__file__).resolve().parents[1] / "cmake/Version.cmake")])
        return subprocess.run(command, env=dict(os.environ, SOURCE_DATE_EPOCH=epoch, TZ="Pacific/Honolulu"),
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_utc_date_and_reproducible_build_epoch(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.strip(), "2026.10.4")

    def test_explicit_version_is_not_replaced_by_build_clock(self):
        result = self.resolve("2026.1.9")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.strip(), "2026.1.9")

    def test_snapshot_retains_label_and_has_numeric_native_metadata(self):
        for version in ("2026.10.5-SNAPSHOT",):
            result = self.resolve(version)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout.strip(), version)
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                f'include("{repo.as_posix()}/cmake/Version.cmake")\n'
                'project(Snapshot VERSION ${SQUADSPEAK_NUMERIC_VERSION} LANGUAGES NONE)\n'
                f'configure_file("{repo.as_posix()}/src/Info.plist.in" bundle.plist)\n')
            configured = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "out"),
                "-DSQUADSPEAK_VERSION=2026.10.5-SNAPSHOT"], capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            metadata = plistlib.loads((root / "out/bundle.plist").read_bytes())
            self.assertEqual(metadata["CFBundleVersion"], "2026.10.5")
            self.assertEqual(metadata["CFBundleShortVersionString"], "2026.10.5")
            self.assertEqual(metadata["NSScreenCaptureUsageDescription"],
                             "SquadSpeak captures a screen, window, or app only when you start screen sharing in a channel.")

    def test_apple_build_number_is_independent_from_the_marketing_version(self):
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                f'include("{repo.as_posix()}/cmake/Version.cmake")\n'
                'project(SquadSpeak VERSION ${SQUADSPEAK_NUMERIC_VERSION} LANGUAGES NONE)\n'
                f'configure_file("{repo.as_posix()}/src/Info.plist.in" bundle.plist)\n')
            configured = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "out"),
                "-DSQUADSPEAK_VERSION=2026.11.18",
                "-DSQUADSPEAK_APPLE_BUILD_NUMBER=44280001"], capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            metadata = plistlib.loads((root / "out/bundle.plist").read_bytes())
            self.assertEqual(metadata["CFBundleShortVersionString"], "2026.11.18")
            self.assertEqual(metadata["CFBundleVersion"], "44280001")

    def test_invalid_apple_build_number_fails_before_configuration(self):
        result = subprocess.run(["cmake", "-DSQUADSPEAK_APPLE_BUILD_NUMBER=1.02", "-P",
            str(Path(__file__).resolve().parents[1] / "cmake/Version.cmake")],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SQUADSPEAK_APPLE_BUILD_NUMBER", result.stdout)

    def test_invalid_versions_fail_before_configuration(self):
        for version in ("", "2026.01.09", "v2026.1.9", "2026.1", "2026.1.9.1", "1.2.3;bad", "1.2.3\n",
                        "1.2.3-SNAPSHOT\n", "1.2.3-SNAPSHOT;bad", "1.2.3-unknown",
                        "1.2.3-SNAPSHOT.17", "1.2.3-SNAPSHOT.0", "1.2.3-SNAPSHOT.01", "1.2.3-SNAPSHOT.1.2"):
            with self.subTest(version=version):
                result = self.resolve(version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("SQUADSPEAK_VERSION", result.stdout)


class UpdateConfigurationTests(unittest.TestCase):
    def configure(self, key, store=True):
        source = Path(__file__).resolve().parents[1] / "cmake/Updates.cmake"
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "check.cmake"
            script.write_text('set(CMAKE_SYSTEM_NAME Darwin)\n'
                              f'set(SQUADSPEAK_STORE_BUILD {"ON" if store else "OFF"})\n'
                              f'include("{source.as_posix()}")\n'
                              'if(squad_update_plist OR TARGET squad_updater)\n'
                              '  message(FATAL_ERROR "Store must not configure external updates")\nendif()\n')
            return subprocess.run(["cmake", "-DSQUADSPEAK_UPDATE_PUBLIC_KEY=" + key, "-P", str(script)],
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_store_omits_updater_even_with_a_public_key(self):
        for key in ("", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="):
            result = self.configure(key)
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_invalid_public_keys_fail_before_sdk_download(self):
        for key in ("short", "A" * 44, "A" * 42 + "==", "A" * 42 + "<=" , "A" * 43 + "=\n"):
            with self.subTest(key=key):
                result = self.configure(key, store=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("32-byte Ed25519 key", result.stdout)


class HomebrewTests(unittest.TestCase):
    def generate(self, directory, version="2026.10.4"):
        return subprocess.run(["cmake", "-DARCHIVES_DIR=" + str(directory),
                               "-DSQUADSPEAK_VERSION=" + version, "-P",
                               str(Path(__file__).resolve().parents[1] / "cmake/BuildCask.cmake")],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_exact_archives_and_cli_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for arch in ("arm64", "x86_64"):
                (root / f"squadspeak-macos-{arch}.zip").write_bytes(arch.encode())
            result = self.generate(root)
            self.assertEqual(result.returncode, 0, result.stdout)
            text = (root / "squadspeak.rb").read_text()
            for arch in ("arm64", "x86_64"):
                self.assertIn(hashlib.sha256(arch.encode()).hexdigest(), text)
                self.assertIn(f"# yuna-release-asset: squadspeak-macos-{arch}.zip", text)
            self.assertIn("strategy :github_latest", text)
            self.assertIn('version "2026.10.4"', text)
            self.assertIn('binary "#{appdir}/squadspeak.app/Contents/MacOS/squadspeak"', text)
            self.assertNotIn("no_check", text)
            self.assertEqual(self.generate(root).returncode, 0)
            self.assertEqual((root / "squadspeak.rb").read_text(), text)

    def test_invalid_input_preserves_existing_cask(self):
        for invalid in ("missing", "empty", "directory", "version", "snapshot"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "squadspeak.rb").write_text("previous release")
                (root / "squadspeak-macos-arm64.zip").write_bytes(b"arm")
                intel = root / "squadspeak-macos-x86_64.zip"
                if invalid == "directory":
                    intel.mkdir()
                elif invalid != "missing":
                    intel.write_bytes(b"" if invalid == "empty" else b"intel")
                version = '1.2.3";invalid' if invalid == "version" else "2026.10.4"
                if invalid == "snapshot": version += "-SNAPSHOT"
                result = self.generate(root, version)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((root / "squadspeak.rb").read_text(), "previous release")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--compile-commands", type=Path)
    options, arguments = parser.parse_known_args()
    if options.compile_commands:
        class CompiledBuildTests(unittest.TestCase):
            def test_large_resources_bypass_cpp_array_compilation(self):
                entries = json.loads(options.compile_commands.read_text(encoding="utf-8"))
                resources = {Path(entry["file"]) for entry in entries
                             if Path(entry["file"]).name.startswith("qrc_")}
                self.assertTrue(resources)
                for resource in resources:
                    # Image atlases must link as objects, not enormous C++ arrays.
                    self.assertLess(resource.stat().st_size, 64 * 1024 * 1024, str(resource))

            def test_snapshot_version_only_invalidates_its_consumers(self):
                repo = Path(__file__).resolve().parents[1]
                consumers = {repo / name for name in ("src/main.cpp", "src/headless.cpp",
                             "tests/headless_tests.cpp", "tests/controls_tests.cpp")}
                entries = json.loads(options.compile_commands.read_text(encoding="utf-8"))
                affected = {Path(entry["file"]).resolve() for entry in entries
                            if "SQUADSPEAK_VERSION=" in entry["command"]}
                self.assertEqual(affected, consumers)

    unittest.main(argv=[sys.argv[0], *arguments])
