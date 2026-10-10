#!/usr/bin/env python3
"""Check an installed macOS bundle without developer library search paths."""
import argparse
import base64
import json
import os
from pathlib import Path
import plistlib
import re
import select
import socket
import struct
import subprocess
import tempfile
import time
import zlib

from check_dependency_notices import check_notices


def version(value):
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise argparse.ArgumentTypeError("Expected a numeric macOS version.")
    return tuple((list(map(int, value.split("."))) + [0, 0])[:3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--minimum-system", required=True, type=version)
    parser.add_argument("--architecture", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--qml-reference", type=Path)
    args = parser.parse_args()
    app = args.app.resolve(strict=True)
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "QML", "DYLD_"))}
    environment.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", QT_QUICK_CONTROLS_STYLE="Basic")
    errors = []
    license_file = app / "Contents/Resources/LICENSE"
    if not license_file.is_file() or license_file.read_bytes() != (Path(__file__).resolve().parents[1] / "LICENSE").read_bytes():
        errors.append({"check": "project_license"})
    announcement = app / "Contents/Resources/announcement.wav"
    source_sound = Path(__file__).resolve().parents[1] / "ui/sounds/announcement.wav"
    if not announcement.is_file() or announcement.read_bytes() != source_sound.read_bytes():
        errors.append({"check": "announcement_sound"})
    for relative in ("Contents/Frameworks/QtTest.framework", "Contents/Frameworks/QtQuickTest.framework",
                     "Contents/Resources/qml/QtTest"):
        if (app / relative).exists():
            errors.append({"check": "test_runtime_in_app", "path": relative})

    def run(arguments, **kwargs):
        return subprocess.run(arguments, env=environment, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=60, **kwargs)

    def check_process(name, result):
        if result.returncode:
            errors.append({"check": name, "exit": result.returncode,
                           "diagnostic": result.stderr.decode(errors="replace")[-4096:]})

    if args.qml_reference:
        # Deployment changes paths and code signatures, but retains the linked
        # Mach-O UUID. Compare the chosen architecture with the corrected build.
        def binary_uuid(path):
            result = run(["/usr/bin/dwarfdump", "--uuid", str(path)])
            check_process("qml_runtime_uuid", result)
            match = re.search(r"UUID: ([0-9A-F-]+) \(" + args.architecture + r"\)",
                              result.stdout.decode())
            return match.group(1) if match else None

        reference = binary_uuid(args.qml_reference)
        installed = binary_uuid(app / "Contents/Frameworks/QtQml.framework/Versions/A/QtQml")
        if not reference or installed != reference:
            errors.append({"check": "corrected_qml_runtime", "expected": reference, "actual": installed})

    # The app and Qt Multimedia must share one patched FFmpeg runtime.
    for library in ("avcodec", "avutil", "swscale", "avformat", "swresample"):
        copies = {path.resolve() for path in (app / "Contents/Frameworks").glob(f"lib{library}.*.dylib")}
        if len(copies) != 1 or not next(iter(copies)).is_relative_to(app):
            errors.append({"check": "ffmpeg_runtime", "library": library, "copies": len(copies)})
        elif library == "avutil" and b"7.1.5-squadspeak.2\0" not in next(iter(copies)).read_bytes():
            errors.append({"check": "ffmpeg_lifetime_fixes"})

    with (app / "Contents/Info.plist").open("rb") as source:
        metadata = plistlib.load(source)
    if "SUPublicEDKey" in metadata:
        try:
            assert len(base64.b64decode(metadata["SUPublicEDKey"], validate=True)) == 32
            assert metadata["SUFeedURL"] == ("https://github.com/YunaBraska/SquadSpeak/releases/latest/download/"
                                              f"appcast-macos-{args.architecture}.xml")
            for key, expected in (("SUEnableAutomaticChecks", True), ("SUAutomaticallyUpdate", False),
                                  ("SUAllowsAutomaticUpdates", False), ("SUVerifyUpdateBeforeExtraction", True),
                                  ("SURequireSignedFeed", True)):
                assert metadata[key] is expected
            framework = app / "Contents/Frameworks/Sparkle.framework"
            assert (framework / "Sparkle").is_file()
            assert (framework / "Versions/B/Updater.app/Contents/MacOS/Updater").is_file()
            assert (framework / "Versions/B/Autoupdate").is_file()
        except (AssertionError, KeyError, ValueError, TypeError):
            errors.append({"check": "signed_update_configuration"})
    notices = app / "Contents/Resources/THIRD_PARTY_NOTICES"
    modules = [framework.stem.removeprefix("Qt")
               for framework in (app / "Contents/Frameworks").glob("Qt*.framework")]
    errors.extend({"check": "qt_module_notices", "diagnostic": error}
                  for error in check_notices(notices, modules))
    sqlite_driver = app / "Contents/PlugIns/sqldrivers/libqsqlite.dylib"
    if not sqlite_driver.is_file():
        errors.append({"check": "qt_sqlite_driver", "path": str(sqlite_driver.relative_to(app))})
    for driver in sqlite_driver.parent.glob("*.dylib"):
        if driver != sqlite_driver:
            errors.append({"check": "unexpected_sql_driver", "path": str(driver.relative_to(app))})
    source_notices = Path(__file__).resolve().parents[1] / "docs/third-party"
    for source in source_notices.rglob("*"):
        if not source.is_file() or source.suffix not in (".txt", ".spdx"):
            continue
        relative = source.relative_to(source_notices)
        installed = notices / relative
        if not installed.is_file() or installed.read_bytes() != source.read_bytes():
            errors.append({"check": "third_party_notice", "file": str(relative)})
    for language in metadata.get("CFBundleLocalizations", []):
        permissions = app / "Contents/Resources" / f"{language}.lproj/InfoPlist.strings"
        localized = run(["plutil", "-convert", "json", "-o", "-", str(permissions)])
        check_process("permission_localization_" + language, localized)
        if localized.returncode == 0:
            entries = json.loads(localized.stdout)
            for key in ("NSMicrophoneUsageDescription", "NSLocalNetworkUsageDescription"):
                if not entries.get(key, "").strip():
                    errors.append({"check": "permission_text", "language": language, "key": key})
    icon_name = metadata.get("CFBundleIconFile", "")
    icon_file = app / "Contents/Resources" / icon_name
    if not icon_name or not icon_file.is_file() or icon_file.read_bytes()[:4] != b"icns":
        errors.append({"check": "app_icon", "file": icon_name})
    declared_minimum = metadata.get("LSMinimumSystemVersion", "")
    if not declared_minimum or version(declared_minimum) > args.minimum_system:
        errors.append({"check": "bundle_minimum_system", "declared": declared_minimum})
    executable = app / "Contents/MacOS" / metadata["CFBundleExecutable"]
    # Qt links permission status and permission requests separately. A bundle can
    # start successfully with the status plugin but still never ask for access.
    permission_methods = run(["otool", "-v", "-s", "__TEXT", "__objc_methname", str(executable)])
    check_process("microphone_permission_methods", permission_methods)
    if b"requestAccessForMediaType:completionHandler:" not in permission_methods.stdout.split():
        errors.append({"check": "microphone_permission_request"})
    with tempfile.TemporaryDirectory(prefix="squadspeak-package-smoke-") as temporary:
        smoke = run([str(executable), "--smoke-test", "--settings-file", temporary + "/audio.ini"])
    check_process("app_start", smoke)

    def packaged_server_run(profile, identity, port, server_environment):
        state = {}
        diagnostic = tempfile.TemporaryFile()
        server = subprocess.Popen([
            str(executable), "--headless", "--settings-file", str(profile),
            "--identity-file", str(identity), "--port", str(port),
            "--channel-name", "Packaged server"], env=server_environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=diagnostic)
        try:
            deadline = time.monotonic() + 15
            pending = b""
            awaiting_status = False
            next_request = 0
            while server.poll() is None and time.monotonic() < deadline:
                if not awaiting_status and time.monotonic() >= next_request:
                    server.stdin.write(b'{"command":"status"}\n')
                    server.stdin.flush()
                    awaiting_status = True
                    next_request = time.monotonic() + 0.25
                if select.select([server.stdout], [], [], 0.25)[0]:
                    pending += os.read(server.stdout.fileno(), 65536)
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        if line:
                            reply = json.loads(line)
                            if reply.get("command") == "status":
                                state = reply.get("data", {})
                                awaiting_status = False
                if state.get("hosting"):
                    break
            if server.poll() is None:
                server.communicate(b'{"command":"quit"}\n', timeout=5)
            diagnostic.seek(0)
            return server.returncode, state, diagnostic.read().decode(errors="replace")[-4096:]
        finally:
            if server.poll() is None:
                server.kill()
                server.wait(timeout=5)
            diagnostic.close()

    with tempfile.TemporaryDirectory(prefix="squadspeak-package-server-") as temporary:
        profile = Path(temporary) / "server"
        identity = Path(temporary) / "identity.pem"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server_environment = dict(environment, QT_QPA_PLATFORM="__headless_platform_must_not_load__",
                                  DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent-squadspeak-test-bus")
        try:
            result, state, diagnostic = packaged_server_run(profile, identity, port, server_environment)
            sqlite_store = Path(str(profile) + ".channel.json.chat.sqlite")
            valid = (result == 0 and state.get("ready") and state.get("hosting")
                     and state.get("channel") == "Packaged server" and state.get("port") == port
                     and state.get("hostParticipants") == [] and state.get("hostClients") == []
                     and not profile.exists() and not Path(str(profile) + ".session.json").exists()
                     and identity.exists() and identity.stat().st_mode & 0o777 == 0o600
                     and sqlite_store.is_file() and sqlite_store.stat().st_size > 0)
            if not valid:
                errors.append({"check": "headless_start", "state": state, "diagnostic": diagnostic})
            if valid:
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    reopen_port = reservation.getsockname()[1]
                result, reopened, diagnostic = packaged_server_run(
                    profile, identity, reopen_port, server_environment)
                if not (result == 0 and reopened.get("ready") and reopened.get("hosting")
                        and reopened.get("port") == reopen_port):
                    errors.append({"check": "headless_reopen", "state": reopened,
                                   "diagnostic": diagnostic})
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            errors.append({"check": "headless_start", "diagnostic": str(error)})

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", 2, 2, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress((b"\0" + b"\x10\x90\xa0\x80" * 2) * 2)) + chunk(b"IEND", b"")
    worker = run([str(app / "Contents/MacOS/squad_image_worker")], input=png)
    check_process("image_worker", worker)
    if not worker.stdout.startswith(b"\x89PNG\r\n\x1a\n"):
        errors.append({"check": "image_worker_png"})
    signature = run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)])
    check_process("signature", signature)
    binaries = []
    for path in (app / "Contents").rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as source:
            magic = source.read(4)
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                         b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            continue
        relative = str(path.relative_to(app))
        build = run(["/usr/bin/vtool", "-arch", args.architecture, "-show-build", str(path)])
        check_process(relative + ":build_version", build)
        minimums = re.findall(r"^\s*minos\s+(\S+)", build.stdout.decode(), re.MULTILINE)
        if not minimums or any(version(item) > args.minimum_system for item in minimums):
            errors.append({"check": "binary_minimum_system", "binary": relative, "minimums": minimums})
        commands = run(["/usr/bin/otool", "-arch", args.architecture, "-l", str(path)])
        check_process(relative + ":load_commands", commands)
        command = ""
        dependencies, rpaths = [], []
        for line in commands.stdout.decode().splitlines():
            words = line.strip().split()
            if len(words) == 2 and words[0] == "cmd":
                command = words[1]
            if command in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB") and words[:1] == ["name"]:
                dependencies.append(line.strip()[5:].rsplit(" (offset", 1)[0])
            if command == "LC_RPATH" and words[:1] == ["path"]:
                rpaths.append(line.strip()[5:].rsplit(" (offset", 1)[0])

        def expand(value):
            return Path(value.replace("@loader_path", str(path.parent)).replace("@executable_path", str(executable.parent)))

        for rpath in rpaths:
            if not expand(rpath).resolve().is_relative_to(app):
                errors.append({"check": "external_library_search_path", "binary": relative, "rpath": rpath})
        for dependency in dependencies:
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            candidates = [expand(dependency)]
            if dependency.startswith("@rpath/"):
                candidates = [expand(item) / dependency[7:] for item in rpaths + ["@executable_path/../Frameworks"]]
            if not any(item.exists() and item.resolve().is_relative_to(app) for item in candidates):
                errors.append({"check": "external_or_missing_library", "binary": relative, "dependency": dependency})
        binaries.append({"path": relative, "minimums": minimums})
    if not binaries:
        errors.append({"check": "missing_binaries"})
    report = dict(app=str(app), architecture=args.architecture, minimum_system=args.minimum_system,
                  binaries=binaries, errors=errors)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(binaries=len(binaries), errors=errors), indent=2))
    return bool(errors)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="squadspeak-package-home-") as home:
        os.environ["CFFIXED_USER_HOME"] = home
        raise SystemExit(main())
