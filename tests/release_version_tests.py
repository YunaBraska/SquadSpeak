"""Exercise release metadata through public CMake scripts without configuring Qt."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ReleaseVersionTests(unittest.TestCase):
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

    def test_invalid_versions_fail_before_configuration(self):
        for version in ("", "2026.01.09", "v2026.1.9", "2026.1", "2026.1.9.1", "1.2.3;bad", "1.2.3\n"):
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
                              f'include("{source}")\n'
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
            self.assertIn('version "2026.10.4"', text)
            self.assertIn('binary "#{appdir}/squadspeak.app/Contents/MacOS/squadspeak"', text)
            self.assertNotIn("no_check", text)
            self.assertEqual(self.generate(root).returncode, 0)
            self.assertEqual((root / "squadspeak.rb").read_text(), text)

    def test_invalid_input_preserves_existing_cask(self):
        for invalid in ("missing", "empty", "directory", "version"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "squadspeak.rb").write_text("previous release")
                (root / "squadspeak-macos-arm64.zip").write_bytes(b"arm")
                intel = root / "squadspeak-macos-x86_64.zip"
                if invalid == "directory":
                    intel.mkdir()
                elif invalid != "missing":
                    intel.write_bytes(b"" if invalid == "empty" else b"intel")
                result = self.generate(root, '1.2.3";invalid' if invalid == "version" else "2026.10.4")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual((root / "squadspeak.rb").read_text(), "previous release")


if __name__ == "__main__":
    unittest.main()
