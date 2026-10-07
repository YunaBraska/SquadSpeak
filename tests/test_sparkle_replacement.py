#!/usr/bin/env python3
"""Exercise Sparkle's real download, rejection, installation and relaunch path."""
import argparse
import functools
import http.server
import json
import pathlib
import plistlib
import shutil
import subprocess
import tempfile
import threading
import time
import uuid

# Public, deterministic fixture key. Never used by a published application.
PUBLIC_KEY = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg="
PRIVATE_KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="


def run(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True)


def stage(source, sparkle, destination, version, feed, identifier, result):
    app = destination / source.name
    shutil.copytree(source, app, symlinks=True)
    metadata_path = app / "Contents/Info.plist"
    metadata = plistlib.loads(metadata_path.read_bytes())
    metadata.update(SUPublicEDKey=PUBLIC_KEY, SUFeedURL=feed,
                    CFBundleIdentifier=identifier, CFBundleVersion=version,
                    CFBundleShortVersionString=version, FixtureResult=str(result),
                    SUEnableAutomaticChecks=True, SUAutomaticallyUpdate=False,
                    SUVerifyUpdateBeforeExtraction=True, SURequireSignedFeed=True)
    metadata_path.write_bytes(plistlib.dumps(metadata))
    framework = app / "Contents/Frameworks/Sparkle.framework"
    framework.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(sparkle / "Sparkle.framework", framework, symlinks=True)
    if version == "2.0.0":
        resources = app / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "replacement-proof.txt").write_text("replacement-proof-v2", encoding="ascii")
    run("codesign", "--force", "--deep", "--sign", "-", app)
    return app


def run_case(args, root, fault):
    web = root / "web"
    web.mkdir()
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(web))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    identifier = "app.squadspeak.update-fixture." + uuid.uuid4().hex
    result = root / "result.json"
    process = None
    log = root / "process.log"
    try:
        port = server.server_address[1]
        feed_url = f"http://127.0.0.1:{port}/appcast.xml"
        current = stage(args.fixture, args.sparkle, root / "installed", "1.0.0", feed_url, identifier, result)
        update = stage(args.fixture, args.sparkle, root / "update", "2.0.0", feed_url, identifier, result)
        archive = web / "update.zip"
        run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", update, archive)
        key = root / "fixture-key"
        key.write_text(PRIVATE_KEY + "\n", encoding="ascii")
        key.chmod(0o600)
        signature = run(args.sign_update, "--ed-key-file", key, "-p", archive).stdout.strip()
        if fault == "archive":
            with archive.open("ab") as stream:
                stream.write(b"tampered")
        feed = web / "appcast.xml"
        feed.write_text(f'''<?xml version="1.0"?><rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"><channel><title>Fixture</title><item><title>2.0.0</title><sparkle:version>2.0.0</sparkle:version><sparkle:shortVersionString>2.0.0</sparkle:shortVersionString><enclosure url="http://127.0.0.1:{port}/update.zip" length="{archive.stat().st_size}" type="application/octet-stream" sparkle:edSignature="{signature}"/></item></channel></rss>''', encoding="utf-8")
        run(args.sign_update, "--ed-key-file", key, "--disable-signing-warning", feed)
        if fault == "feed":
            feed.write_bytes(feed.read_bytes().replace(b"<title>Fixture</title>", b"<title>Changed</title>"))
        with log.open("w") as output:
            process = subprocess.Popen([str(current / "Contents/MacOS/sparkle_replacement_fixture")],
                                       stdout=output, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 45
            # The original process must quit so the installer can replace it.
            # Success is evidence from the relaunched copy, not the original exit.
            while not result.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not result.exists():
                raise AssertionError("No installer result")
            proof = json.loads(result.read_text())
            installed = plistlib.loads((current / "Contents/Info.plist").read_bytes())
            if fault:
                assert proof["event"] == "rejected", proof
                assert 3002 in proof["codes"], proof  # SUValidationError from EdDSA verification.
                assert installed["CFBundleVersion"] == "1.0.0", installed
                assert not (current / "Contents/Resources/replacement-proof.txt").exists()
            else:
                assert proof == {"event": "relaunched", "version": "2.0.0", "marker": "replacement-proof-v2"}, proof
                assert installed["CFBundleVersion"] == "2.0.0", installed
            print(f"Sparkle {fault or 'replacement'}: passed", flush=True)
    except Exception as error:
        raise AssertionError(f"{fault or 'replacement'} failed: {error}\n{log.read_text() if log.exists() else ''}") from error
    finally:
        if process is not None:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        server.shutdown()
        server.server_close()
        thread.join()
        subprocess.run(["defaults", "delete", identifier], capture_output=True)
        shutil.rmtree(pathlib.Path.home() / "Library/Caches" / identifier, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--sparkle", type=pathlib.Path, required=True)
    parser.add_argument("--sign-update", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="squadspeak-update-proof-") as directory:
        for fault in (None, "archive", "feed"):
            root = pathlib.Path(directory) / (fault or "replacement")
            root.mkdir()
            run_case(args, root, fault)


if __name__ == "__main__":
    main()
