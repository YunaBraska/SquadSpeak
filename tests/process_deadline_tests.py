"""Exercise the deadline runner using real child processes, including cleanup."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


class ProcessDeadlineTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.dump = self.root / "threads.dmp"

    def run_test(self, *command, timeout="5"):
        return subprocess.run([sys.executable, str(Path(__file__).with_name("run_with_deadline.py")),
                               "--timeout", timeout, "--dump", str(self.dump), *command],
                              capture_output=True, text=True, timeout=20)

    def test_success_and_exit_status_are_preserved(self):
        for status in (0, 17):
            with self.subTest(status=status):
                self.dump.write_bytes(b"stale evidence")
                result = self.run_test(sys.executable, "-c", f"print('ready'); raise SystemExit({status})")
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn("ready", result.stdout)
                self.assertFalse(self.dump.exists())

    def test_missing_program_and_invalid_deadline_fail(self):
        result = self.run_test(str(self.root / "missing-program"))
        self.assertEqual(result.returncode, 127, result.stderr)
        self.assertIn("Could not start test", result.stderr)
        for deadline in ("0", "-1", "nan", "inf"):
            with self.subTest(deadline=deadline):
                result = self.run_test(sys.executable, timeout=deadline)
                self.assertEqual(result.returncode, 2, result.stderr)

    def test_hang_is_dumped_and_child_is_reaped(self):
        result = self.run_test(sys.executable, "-c",
                               "import threading; threading.Event().wait()", timeout="2")
        self.assertEqual(result.returncode, 124, result.stderr)
        pid = int(re.search(r"Test exceeded 2 seconds, process (\d+)", result.stderr)[1])
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.OpenProcess(0x100000, False, pid)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 0), 0)
                finally:
                    kernel.CloseHandle(handle)
            self.assertEqual(self.dump.read_bytes()[:4], b"MDMP", result.stderr)
        else:
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            self.assertFalse(self.dump.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--controls-tests", type=Path)
    options, arguments = parser.parse_known_args()
    if options.controls_tests:
        class ControlsStartupTests(unittest.TestCase):
            def test_visible_window_passes_and_hidden_window_fails_without_hanging(self):
                with tempfile.TemporaryDirectory() as directory:
                    fixture = Path(directory) / "startup.qml"
                    for hide in (False, True):
                        with self.subTest(hidden=hide):
                            fixture.write_text("""import QtQuick
import QtTest

Item {
    id: fixture
    width: 400
    height: 200
    Connections {
        target: fixture.Window.window
        function onVisibleChanged() {
            if (HIDE && target.visible) target.visible = false
        }
    }
    TestCase {
        name: "Startup"
        when: windowShown
        function test_reached() { console.warn("ordinary warning is preserved"); verify(true) }
    }
}
""".replace("HIDE", str(hide).lower()), encoding="utf-8")
                            result = subprocess.run(
                                [str(options.controls_tests.resolve()), "-input", str(fixture)],
                                env={**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software",
                                     "TMPDIR": directory, "TMP": directory, "TEMP": directory},
                                capture_output=True, text=True, timeout=15)
                            output = result.stderr + result.stdout
                            if hide:
                                self.assertEqual(result.returncode, 1, output)
                                self.assertIn("QtQuickTest startup failure", output)
                            else:
                                self.assertEqual(result.returncode, 0, output)
                                self.assertIn("ordinary warning is preserved", output)

    unittest.main(argv=[sys.argv[0], *arguments])
