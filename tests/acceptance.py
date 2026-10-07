"""Prepare an offline device-check handoff; never infer physical passes from CI."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import tempfile
import xml.etree.ElementTree as ET


# Unclassified runtime/build inputs affect every check. Keep exclusions narrow.
SOURCE_GROUPS = {
    "audio": {"audio_model", "audio_processor", "audio_profiles", "audio_recording",
              "echo_canceller", "output_monitor", "voice_mixer"},
    "notifications": {"apple_notifications"},
    "video": {"screen_share", "screen_capture", "video_codec"},
}
CHECKS = [
    dict(id="installation", title="Windows-Paket starten", minutes=1,
         platforms=["windows"], groups=list(SOURCE_GROUPS),
         steps=["Das bereitgestellte ZIP entpacken. Falls Windows nach der C++-Laufzeit fragt: bin/vc_redist.x64.exe einmal starten.",
                "bin/squadspeak.exe öffnen. Über das Tray-Menü mit Quit beenden, dann erneut öffnen."],
         expected="Die Channel-Ansicht öffnet sich beide Male, ohne fehlende DLL oder Absturz."),
    dict(id="listening", title="Stimme anhören", minutes=1,
         platforms=["macos", "windows", "linux"], groups=["audio"],
         steps=["Die vorbereiteten Apps A und B sind im selben Channel. Kopfhörer tragen. Nur A darf das Mikrofon nutzen; nur B gibt Ton aus.",
                "A freischalten. Langsam sagen: 'Das Fenster ist offen. Eins, zwei, drei, vier, fünf.' Dann fünf Sekunden still sein und A wieder stummschalten."],
         expected="In B ist der Satz vollständig verständlich. Kein endloses Echo, Knacken oder verschluckter Anfang. Die verzögerte eigene Stimme ist in diesem Test beabsichtigt."),
    dict(id="devices", title="Headset einmal neu verbinden", minutes=1,
         platforms=["macos", "windows", "linux"], groups=["audio"],
         steps=["Nur mit vorhandenem USB-/Bluetooth-Headset durchführen; sonst 'Nicht möglich' wählen. Zuerst beide Apps stummschalten.",
                "Headset trennen und erneut verbinden. In A Settings > Input öffnen, das Headset wählen und 'Eins, zwei, drei' sagen. Danach Input verlassen.",
                "In B Settings > Output das Headset wählen und den Tontest starten."],
         expected="A zeigt wieder Mikrofonbewegung. In B ist der Testton hörbar. Keine App muss neu gestartet werden. Bei Bluetooth bitte das Modell in die Notiz schreiben."),
    dict(id="notifications", title="Nachricht mit und ohne Fokus", minutes=1,
         platforms=["macos"], groups=["notifications", "audio"],
         steps=["A und B bleiben im selben Sprachchannel, Mikrofone stumm. macOS-Fokus ausschalten, Mitteilungen für B erlauben. Von A 'Test 1' senden.",
                "Bei B auf genau einen Nachrichtenton und die macOS-Mitteilung achten. Danach Fokus einschalten und von A 'Test 2' senden."],
         expected="Ohne Fokus erscheint die Mitteilung und der Nachrichtenton spielt einmal. Mit Fokus entspricht die Mitteilung deinen macOS-Ausnahmen; der App-Ton darf weiter spielen."),
]

WINDOWS_LAUNCHER = r'''@echo off
setlocal DisableDelayedExpansion
set "app=%~dp0..\bin\squadspeak.exe"
if not exist "%app%" goto missing
set QT_FORCE_STDERR_LOGGING=1
if /i "%~1"=="--smoke-test" goto smoke
start "" "%~dp0Check.html"
start "" "%app%" --settings --settings-file "%~dp0A\audio.ini" > "%~dp0A\app.log" 2>&1
start "" "%app%" --settings --settings-file "%~dp0B\audio.ini" > "%~dp0B\app.log" 2>&1
exit /b 0
:smoke
"%app%" --smoke-test --settings-file "%~dp0A\audio.ini"
if errorlevel 1 exit /b 1
"%app%" --smoke-test --settings-file "%~dp0B\audio.ini"
exit /b %errorlevel%
:missing
echo SquadSpeak is missing. Extract the entire ZIP before opening device-test\Start.cmd.
if /i "%~1"=="--smoke-test" exit /b 1
pause
exit /b 1
'''


def windows_kit(package, html):
    # Produce a companion to the real package, not another app or test runtime.
    # Never reuse an existing kit: it may contain live identities and observations.
    with tempfile.TemporaryDirectory(dir=package) as temporary:
        staged = Path(temporary) / "device-test"
        staged.mkdir()
        for tag, name, port, avatar, palette in (
                ("A", "Ari", 48764, "mossling", "ocean"),
                ("B", "Bea", 48765, "courier", "forest")):
            profile = staged / tag
            profile.mkdir()
            session = dict(version=1, userName=name,
                           muted=True, deafened=True, avatar=avatar, theme="system",
                           palette=palette, language="en")
            channel = dict(version=1, channelName=f"{name} - local test", approved=[], attempts={}, requestsAllowed=True, servicePort=port)
            for suffix, content in (("session", session), ("channel", channel)):
                (profile / f"audio.ini.{suffix}.json").write_text(json.dumps(content) + "\n", encoding="utf-8")
        (staged / "Start.cmd").write_bytes(WINDOWS_LAUNCHER.replace("\n", "\r\n").encode("ascii"))
        (staged / "Check.html").write_text(html, encoding="utf-8")
        staged.rename(package / "device-test")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_inputs(root):
    paths = [root / "CMakeLists.txt"]
    for folder in ("src", "ui", "cmake", ".github/workflows"):
        paths.extend((root / folder).rglob("*"))
    paths.extend((root / "tests").glob("*ci*"))
    return {p.relative_to(root).as_posix(): digest(p.read_bytes())
            for p in sorted(set(paths)) if p.is_file()}


def fingerprint(check, inputs):
    relevant = {}
    for path, value in inputs.items():
        group = next((group for group, stems in SOURCE_GROUPS.items()
                      if path.startswith("src/") and any(
                          Path(path).stem == stem or Path(path).stem.startswith(stem + "_") for stem in stems)), None)
        if group is None or group in check["groups"]:
            relevant[path] = value
    return digest(json.dumps([SOURCE_GROUPS_FOR_HASH, check, relevant], sort_keys=True).encode())


SOURCE_GROUPS_FOR_HASH = {key: sorted(value) for key, value in SOURCE_GROUPS.items()}


def automated_report(path):
    data = path.read_bytes()
    tree = ET.fromstring(data)
    cases = []
    for case in tree.iter("testcase"):
        output = "\n".join(case.itertext())
        result = "pass"
        if case.find("failure") is not None or case.find("error") is not None:
            result = "fail"
        elif (case.find("skipped") is not None or case.get("status") in ("notrun", "disabled")
              or re.search(r"(?m)^\s*SKIP\s*:", output)):
            result = "skipped"
        cases.append({"name": case.get("name", "unnamed"), "result": result})
    if not cases:
        raise ValueError(f"No test results in {path}")
    return {"file": path.name, "sha256": digest(data), "cases": cases,
            "counts": {state: sum(c["result"] == state for c in cases) for state in ("pass", "fail", "skipped")}}


def previous_checks(path, platform, setup, runtime):
    if path is None:
        return {}
    previous = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(previous, dict) or previous.get("schema") != 1 or not isinstance(previous.get("checks"), list):
        raise ValueError("Unsupported or incomplete device-check receipt")
    checks = {}
    for check in previous["checks"]:
        if not isinstance(check, dict) or not isinstance(check.get("id"), str) or check["id"] in checks:
            raise ValueError("Invalid or repeated check in receipt")
        if check.get("result") not in ("pending", "pass", "fail", "blocked"):
            raise ValueError("Invalid result in receipt")
        for field in ("fingerprint", "checkedAt", "testedBuild", "note"):
            if not isinstance(check.get(field), str):
                raise ValueError(f"Missing {field} in receipt")
        if check["result"] != "pending":
            if not check["checkedAt"] or not check["testedBuild"]:
                raise ValueError("A completed observation needs a timestamp and build")
            checked = datetime.fromisoformat(check["checkedAt"].replace("Z", "+00:00"))
            if checked.utcoffset() != timezone.utc.utcoffset(None) or checked > datetime.now(timezone.utc):
                raise ValueError("Receipt timestamps must be past UTC times")
        checks[check["id"]] = check
    return checks if (runtime and previous.get("platform") == platform
                      and previous.get("setup") == setup and previous.get("runtime") == runtime) else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--platform", choices=("macos", "windows", "linux"), required=True)
    parser.add_argument("--setup", required=True, help="Exact OS version, computer and input/output devices; no serial numbers.")
    parser.add_argument("--build", required=True, help="Build/commit actually supplied to the tester.")
    parser.add_argument("--runtime", default="", help="Verified toolchain/dependency/build-preset identity; omitted means no result reuse.")
    parser.add_argument("--junit", type=Path, action="append", default=[])
    parser.add_argument("--previous", type=Path, help="Previously downloaded results; matching observations are retained.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-package", type=Path,
                        help="Add a portable two-app device-test folder to an extracted Windows package.")
    args = parser.parse_args()
    try:
        if ((not args.setup.strip() and not args.windows_package) or not args.build.strip()
                or not (args.source / "src").is_dir()):
            raise ValueError("A source checkout, nonempty setup and build are required")
        if args.windows_package and (args.platform != "windows"
                or not (args.windows_package / "bin/squadspeak.exe").is_file()
                or (args.windows_package / "device-test").exists()):
            raise ValueError("Windows kit needs bin/squadspeak.exe and no existing device-test folder")
        inputs = source_inputs(args.source)
        previous = previous_checks(args.previous, args.platform, args.setup, args.runtime)
        checks = []
        for definition in CHECKS:
            if args.platform not in definition["platforms"]:
                continue
            if args.windows_package and definition["id"] == "installation":
                definition = dict(definition, steps=[
                    "Das ganze ZIP entpacken und device-test/Start.cmd öffnen. Es öffnet die Einstellungen von Ari und Bea; jeweils mit Done zur Channel-Ansicht wechseln.",
                    "Beide Apps über das Tray-Menü mit Quit beenden. Start.cmd erneut öffnen. Die normalen App-Profile nicht starten."],
                    expected="Beide Channel-Ansichten öffnen sich beide Male, ohne fehlende DLL oder Absturz.")
            check = dict(definition, fingerprint=fingerprint(definition, inputs), result="pending",
                         checkedAt="", testedBuild="", note="")
            old = previous.get(check["id"], {})
            if old.get("fingerprint") == check["fingerprint"]:
                check.update({key: old[key] for key in ("result", "checkedAt", "testedBuild", "note")})
            checks.append(check)
        report = dict(schema=1, platform=args.platform, setup=args.setup, build=args.build, runtime=args.runtime,
                      standaloneWindows=bool(args.windows_package),
                      preparedAt=datetime.now(timezone.utc).isoformat(), checks=checks,
                      automated=[automated_report(path) for path in args.junit])
        template = Path(__file__).with_suffix(".html").read_text(encoding="utf-8")
        data = json.dumps(report, ensure_ascii=True).replace("<", "\\u003c")
        html = template.replace("REPORT_DATA", data)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=args.output.parent) as pending:
            temporary = Path(pending) / "report.html"
            temporary.write_text(html, encoding="utf-8")
            temporary.chmod(0o600)
            temporary.replace(args.output)
        if args.windows_package:
            windows_kit(args.windows_package, html)
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"Device check: {error}\n")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
