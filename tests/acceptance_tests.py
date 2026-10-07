"""Exercise the handoff report through its public command, without Qt or devices."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "src").mkdir()
        (self.root / "src/audio_model.cpp").write_text("audio", encoding="utf-8")
        (self.root / "src/apple_notifications.mm").write_text("notifications", encoding="utf-8")
        self.output = self.root / "report.html"

    def run_report(self, *extra, success=True):
        result = subprocess.run([sys.executable, "-X", "utf8", str(Path(__file__).with_name("acceptance.py")),
            "--source", str(self.root), "--platform", "macos", "--setup", "Mac / OS 26 / wired headset",
            "--build", "preview-test", "--runtime", "test-kit", "--output", str(self.output), *map(str, extra)],
            encoding="utf-8", capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        if success:
            return json.loads(self.output.read_text(encoding="utf-8").split('<script id="report" type="application/json">')[1].split('</script>')[0])
        return result

    def receipt(self, report):
        for check in report["checks"]:
            check.update(result="pass", checkedAt="2026-01-01T12:00:00Z", testedBuild="preview-test", note="heard it")
        path = self.root / "receipt.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        return path

    def test_missing_automation_and_hardware_are_never_passes(self):
        report = self.run_report()
        self.assertEqual(report["automated"], [])
        self.assertTrue(all(c["result"] == "pending" for c in report["checks"]))
        self.assertEqual({c["id"] for c in report["checks"]}, {"listening", "devices", "notifications"})

    def test_reuse_ignores_documentation_but_invalidates_relevant_code_and_setup(self):
        original = self.run_report()
        receipt = self.receipt(original)
        (self.root / "README.md").write_text("documentation change", encoding="utf-8")
        report = self.run_report("--previous", receipt, "--build", "next-preview")
        self.assertTrue(all(c["result"] == "pass" and c["testedBuild"] == "preview-test" for c in report["checks"]))
        (self.root / "src/apple_notifications.mm").write_text("different notifications", encoding="utf-8")
        report = self.run_report("--previous", receipt)
        self.assertEqual([c["result"] for c in report["checks"]], ["pass", "pass", "pending"])
        (self.root / "src/audio_model.cpp").write_text("different audio", encoding="utf-8")
        report = self.run_report("--previous", receipt)
        self.assertTrue(all(c["result"] == "pending" for c in report["checks"]))
        (self.root / "src/new_runtime.cpp").write_text("unknown dependency", encoding="utf-8")
        self.assertTrue(all(c["result"] == "pending" for c in self.run_report("--previous", receipt)["checks"]))
        self.assertTrue(all(c["result"] == "pending" for c in self.run_report("--previous", receipt, "--setup", "other device")["checks"]))

    def test_build_environment_and_deleted_sources_invalidate_results(self):
        receipt = self.receipt(self.run_report())
        for runtime in ("", "different Qt/compiler"):
            self.assertTrue(all(c["result"] == "pending" for c in
                self.run_report("--previous", receipt, "--runtime", runtime)["checks"]))
        (self.root / "src/audio_model.cpp").unlink()
        self.assertEqual(self.run_report("--previous", receipt)["checks"][0]["result"], "pending")
        (self.root / "CMakeLists.txt").write_text("new dependencies", encoding="utf-8")
        self.assertTrue(all(c["result"] == "pending" for c in self.run_report("--previous", receipt)["checks"]))

    def test_failed_and_blocked_observations_are_preserved_without_becoming_passes(self):
        report = self.run_report()
        path = self.receipt(report)
        report["checks"][0]["result"] = "fail"
        report["checks"][1]["result"] = "blocked"
        path.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual([c["result"] for c in self.run_report("--previous", path)["checks"]], ["fail", "blocked", "pass"])

    def test_junit_failure_skip_and_nested_qt_skip_remain_visible(self):
        xml = self.root / "ctest.xml"
        xml.write_text('<testsuite><testcase name="good"/><testcase name="bad"><failure/></testcase>'
                       '<testcase name="absent"><skipped/></testcase><testcase name="qt">'
                       '<system-out>SKIP   : hidden hardware case</system-out></testcase></testsuite>', encoding="utf-8")
        report = self.run_report("--junit", xml)
        self.assertEqual(report["automated"][0]["counts"], {"pass": 1, "fail": 1, "skipped": 2})
        for content in ("broken XML", "<testsuite/>"):
            before = self.output.read_bytes()
            xml.write_text(content, encoding="utf-8")
            self.run_report("--junit", xml, success=False)
            self.assertEqual(self.output.read_bytes(), before)

    def test_invalid_receipt_and_script_text_are_not_trusted(self):
        path = self.root / "receipt.json"
        path.write_text('{"schema": 999}', encoding="utf-8")
        self.run_report("--previous", path, success=False)
        for bad in ({"id": "listening", "result": "pending"}, {"id": "listening", "result": "pass"}):
            path.write_text(json.dumps({"schema": 1, "checks": [bad]}), encoding="utf-8")
            self.run_report("--previous", path, success=False)
        report = self.run_report()
        path = self.receipt(report)
        for timestamp in ("not a date", "2099-01-01T00:00:00Z", "2026-01-01T12:00:00"):
            report["checks"][0]["checkedAt"] = timestamp
            path.write_text(json.dumps(report), encoding="utf-8")
            self.run_report("--previous", path, success=False)
        hostile = '</script><script>alert("no")</script>'
        report = self.run_report("--setup", hostile)
        self.assertEqual(report["setup"], hostile)
        self.assertNotIn(hostile, self.output.read_text(encoding="utf-8"))

    def test_windows_handoff_includes_installation_without_apple_notification_step(self):
        report = self.run_report("--platform", "windows")
        self.assertEqual({c["id"] for c in report["checks"]}, {"installation", "listening", "devices"})

    def test_windows_kit_is_portable_quiet_and_never_overwrites_profiles(self):
        package = self.root / "Windows package with spaces"
        (package / "bin").mkdir(parents=True)
        (package / "bin/squadspeak.exe").write_bytes(b"fixture; native startup is checked by Windows CI")
        report = self.run_report("--platform", "windows", "--windows-package", package, "--setup", "")
        kit = package / "device-test"
        self.assertEqual((kit / "Check.html").read_bytes(), self.output.read_bytes())
        self.assertTrue(report["standaloneWindows"])
        self.assertEqual(report["setup"], "")
        for tag, port, name in (("A", 48764, "Ari"), ("B", 48765, "Bea")):
            session = json.loads((kit / tag / "audio.ini.session.json").read_text(encoding="utf-8"))
            channel = json.loads((kit / tag / "audio.ini.channel.json").read_text(encoding="utf-8"))
            self.assertEqual(session["userName"], name)
            self.assertTrue(session["muted"] and session["deafened"])
            self.assertEqual(channel["servicePort"], port)
        launcher = (kit / "Start.cmd").read_text(encoding="utf-8")
        self.assertIn('setlocal DisableDelayedExpansion', launcher)
        self.assertIn('%~dp0..\\bin\\squadspeak.exe', launcher)
        self.assertNotIn(str(self.root), launcher)
        self.assertIn('--smoke-test', launcher)
        self.assertEqual(launcher.count(' --settings --settings-file '), 2)
        profile = kit / "A/audio.ini.session.json"
        profile.write_text("existing user's test profile", encoding="utf-8")
        self.run_report("--platform", "windows", "--windows-package", package, success=False)
        self.assertEqual(profile.read_text(encoding="utf-8"), "existing user's test profile")

    def test_kit_rejects_wrong_platform_and_missing_package_before_writing(self):
        for flags in ((), ("--platform", "windows")):
            self.run_report(*flags, "--windows-package", self.root, success=False)
            self.assertFalse((self.root / "device-test").exists())
            self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
