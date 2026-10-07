#!/usr/bin/env python3
import base64
import pathlib
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile


BUILDER = pathlib.Path(__file__).resolve().parents[1] / "cmake" / "build_appcasts.py"
SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
PUBLIC_KEY = base64.b64encode(bytes(range(32))).decode()
SIGNATURE = base64.b64encode(bytes(range(64))).decode()
APP_ID = re.search(r'MACOSX_BUNDLE_GUI_IDENTIFIER "([^"]+)"',
                   (BUILDER.parents[1] / "ui/CMakeLists.txt").read_text()).group(1)


class AppcastBuilderTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.output = self.root / "feeds"
        self.archives = {arch: self.root / f"squadspeak-macos-{arch}.zip" for arch in ("arm64", "x86_64")}
        for archive in self.archives.values():
            self.write_archive(archive)

    @staticmethod
    def write_archive(path, public_key=PUBLIC_KEY, **metadata):
        with zipfile.ZipFile(path, "w") as bundle:
            # Sparkle also includes nested applications with their own metadata.
            bundle.writestr("squadspeak.app/Contents/Frameworks/Sparkle.framework/Versions/B/Updater.app/Contents/Info.plist",
                            plistlib.dumps({"CFBundleIdentifier": "org.sparkle-project.Updater"}))
            bundle.writestr("squadspeak.app/Contents/Info.plist", plistlib.dumps({
                "CFBundleIdentifier": APP_ID, "SUPublicEDKey": public_key,
                "CFBundleVersion": "2026.10.4", "CFBundleShortVersionString": "2026.10.4", **metadata}))

    def build(self, **overrides):
        options = {"version": "2026.10.4", "repository": "YunaBraska/SquadSpeak", "output-dir": self.output,
                   "public-key": PUBLIC_KEY, "arm-archive": self.archives["arm64"], "arm-signature": SIGNATURE,
                   "x86-archive": self.archives["x86_64"], "x86-signature": SIGNATURE}
        options.update(overrides)
        return subprocess.run([sys.executable, str(BUILDER)] +
                              [argument for key, value in options.items() for argument in (f"--{key}", str(value))],
                              capture_output=True, text=True)

    def assert_rejected(self, diagnostic, **options):
        result = self.build(**options)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(diagnostic, result.stderr)
        self.assertEqual(list(self.output.glob("*")), [])

    def test_builds_architecture_feeds(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        for architecture, archive in self.archives.items():
            tree = ET.parse(self.output / f"appcast-macos-{architecture}.xml")
            enclosure = tree.find("./channel/item/enclosure")
            self.assertIsNotNone(enclosure)
            self.assertEqual(enclosure.attrib["length"], str(archive.stat().st_size))
            self.assertEqual(enclosure.attrib["url"],
                             f"https://github.com/YunaBraska/SquadSpeak/releases/download/2026.10.4/{archive.name}")
            self.assertEqual(enclosure.attrib[f"{{{SPARKLE}}}edSignature"], SIGNATURE)
            self.assertEqual(tree.findtext(f"./channel/item/{{{SPARKLE}}}version"), "2026.10.4")
            self.assertEqual(tree.findtext(f"./channel/item/{{{SPARKLE}}}minimumSystemVersion"), "13.0")

    def test_rejects_bad_options_without_partial_output(self):
        for options, diagnostic in (
            ({"arm-signature": "bad"}, "signature"),
            ({"x86-signature": "invalid"}, "signature"),
            ({"arm-archive": self.root / "wrong-name.zip"}, "archive name"),
            ({"version": "2026.10"}, "version"),
            ({"repository": "unexpected/repository"}, "repository"),
            ({"public-key": ""}, "32 bytes"),
            ({"public-key": "invalid!"}, "public key"),
        ):
            with self.subTest(options=options):
                self.assert_rejected(diagnostic, **options)

    def test_rejects_empty_archive(self):
        self.archives["arm64"].write_bytes(b"")
        self.assert_rejected("empty")

    def test_rejects_missing_archive(self):
        self.archives["x86_64"].unlink()
        self.assert_rejected("missing")

    def test_rejects_corrupt_archive(self):
        self.archives["arm64"].write_bytes(b"not a zip file")
        self.assert_rejected("cannot inspect")

    def test_rejects_mismatched_embedded_key(self):
        self.write_archive(self.archives["x86_64"], base64.b64encode(bytes(reversed(range(32)))).decode())
        self.assert_rejected("SUPublicEDKey")

    def test_rejects_wrong_app_or_version(self):
        for field, value in (("CFBundleIdentifier", "another.app"), ("CFBundleVersion", "2026.10.3"),
                             ("CFBundleShortVersionString", "2026.10.3")):
            with self.subTest(field=field):
                self.write_archive(self.archives["x86_64"], **{field: value})
                self.assert_rejected(field)


if __name__ == "__main__":
    unittest.main()
