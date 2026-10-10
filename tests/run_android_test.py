"""Run an Android contract and reject crashes after QtTest reports success."""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("a test runner command is required")
    args.log.unlink(missing_ok=True)
    try:
        # Use the device clock. Do not clear logs belonging to another app.
        start = subprocess.check_output(
            [args.adb, "shell", "date", "+%Y-%m-%d\\ %H:%M:%S.%3N"],
            text=True, timeout=15).strip()
        result = subprocess.run(args.command, check=False)
        logs = subprocess.check_output(
            [args.adb, "logcat", "-d", "-b", "main", "-b", "system", "-b", "crash",
             "-v", "brief", "-T", start], text=True, timeout=15)
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text(logs, encoding="utf-8")
        pids = set(re.findall(r"Start proc (\d+):" + re.escape(args.package) + r"/", logs))
        crashes = [line for line in logs.splitlines()
                   if (match := re.match(r"F/.*?\(\s*(\d+)\):", line)) and match[1] in pids]
        if crashes:
            print("Android process crashed after launch:\n" + "\n".join(crashes), file=sys.stderr)
            return 1
        if result.returncode == 0 and not pids:
            print("Missing Android process-start evidence. See " + str(args.log), file=sys.stderr)
            return 1
        return result.returncode
    except (OSError, subprocess.SubprocessError) as error:
        print(f"Android test or process-log capture failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
