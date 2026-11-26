"""Exercise the package capability gate with real macOS code signatures."""
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


class MacPermissionsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="squadspeak-permissions-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = Path(__file__).resolve().parents[1]
        self.app = self.root / "Fixture.app"
        (self.app / "Contents/MacOS").mkdir(parents=True)
        self.metadata = {"CFBundleExecutable": "fixture", "CFBundlePackageType": "APPL",
                         "CFBundleIdentifier": "app.squadspeak.permissions-fixture",
                         "NSMicrophoneUsageDescription": "Microphone package contract."}
        self.entitlements = plistlib.loads((self.repo / "scripts/macos.entitlements").read_bytes())
        subprocess.run(["xcrun", "clang", "-x", "c", "-", "-o",
                        str(self.app / "Contents/MacOS/fixture")],
                       input="int main(void) { return 0; }", text=True, check=True,
                       capture_output=True, timeout=30)

    def sign(self, entitlements, runtime=True):
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps(self.metadata))
        command = ["codesign", "--force", "--sign", "-", "--timestamp=none"]
        if runtime:
            command += ["--options", "runtime"]
        if entitlements is not None:
            path = self.root / "capabilities.plist"
            path.write_bytes(plistlib.dumps(entitlements))
            command += ["--entitlements", str(path)]
        subprocess.run(command + [str(self.app)], check=True, capture_output=True, timeout=15)

    def check(self, success):
        report = self.root / "report.json"
        result = subprocess.run([sys.executable, str(self.repo / "tests/check_macos_bundle.py"),
                                 str(self.app), "--minimum-system", "13.0", "--architecture",
                                 os.uname().machine, "--report", str(report), "--permissions-only"],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0 if success else 1, result.stdout + result.stderr)
        return json.loads(report.read_text())["errors"]

    def test_valid_ad_hoc_and_hardened_packages_pass(self):
        for runtime in (False, True):
            with self.subTest(runtime=runtime):
                self.sign(self.entitlements, runtime)
                self.assertEqual(self.check(True), [])

    def test_valid_signature_without_entitlements_is_rejected(self):
        self.sign(None)
        errors = self.check(False)
        self.assertEqual({item["key"] for item in errors}, set(self.entitlements))

    def test_disabled_missing_and_wrong_type_capabilities_are_rejected(self):
        for key in self.entitlements:
            for value in (None, False, "true", 1):
                with self.subTest(key=key, value=value):
                    values = dict(self.entitlements)
                    if value is None:
                        del values[key]
                    else:
                        values[key] = value
                    self.sign(values)
                    self.assertEqual(self.check(False), [{"check": "required_entitlement", "key": key}])

    def test_missing_or_invalid_microphone_usage_is_rejected(self):
        for usage in (None, " ", False):
            with self.subTest(usage=usage):
                self.metadata.pop("NSMicrophoneUsageDescription", None)
                if usage is not None:
                    self.metadata["NSMicrophoneUsageDescription"] = usage
                self.sign(self.entitlements)
                self.assertEqual(self.check(False), [{"check": "microphone_usage_description"}])

    def test_modified_signed_bundle_is_rejected(self):
        self.sign(self.entitlements)
        self.metadata["NSMicrophoneUsageDescription"] = "Modified after signing."
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps(self.metadata))
        self.assertIn("signature", [item["check"] for item in self.check(False)])


if __name__ == "__main__":
    unittest.main()
