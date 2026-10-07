#!/usr/bin/env python3
"""Offline contract tests for cmake/build_sources.py."""

from __future__ import annotations

import hashlib
import json
import os
import ssl
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "cmake" / "build_sources.py"


class SourceArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="source-archives-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "cmake").mkdir()
        (self.root / "tests").mkdir()
        self.payload = b"small deterministic source archive\n"
        digest = hashlib.sha256(self.payload).hexdigest()
        self.url = "https://example.invalid/source.tar.xz"
        self.manifest = {
            "qt_version": "6.10.2",
            "archives": [{"name": "source.tar.xz", "url": self.url, "sha256": digest}],
        }
        (self.root / "cmake/source_archives.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        for relative in ("CMakeLists.txt", "cmake/AudioProcessing.cmake", "cmake/Updates.cmake"):
            (self.root / relative).write_text(
                f"FetchContent_Declare(x URL {self.url} URL_HASH SHA256={digest})\n", encoding="utf-8"
            )
        (self.root / "tests/macos-ci.sh").write_text(
            f"fetch source.tar.xz {self.url} {digest}\n", encoding="utf-8"
        )
        (self.root / "tests/windows-ci.ps1").write_text(
            f'Get-Source "source.tar.xz" "{self.url}" "{digest}"\n', encoding="utf-8"
        )
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.email", "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.name", "Source Tests"], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "commit.gpgsign=false", "commit", "-qm", "fixture"], check=True)
        subprocess.run(["git", "-C", str(self.root), "remote", "add", "origin", "https://example.invalid/repo.git"], check=True)

    def run_tool(self, *args: str, environment=None) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args], text=True, capture_output=True, timeout=20, env=environment)

    def test_check_and_deterministic_offline_package(self) -> None:
        checked = self.run_tool("--check")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        cache = self.root / "cache"
        cache.mkdir()
        (cache / "source.tar.xz").write_bytes(self.payload)
        first = self.root / "one.tar.gz"
        second = self.root / "two.tar.gz"
        self.assertEqual(self.run_tool("--cache", str(cache), "--output", str(first)).returncode, 0)
        self.assertEqual(self.run_tool("--cache", str(cache), "--output", str(second)).returncode, 0)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(first, "r:gz") as archive:
            self.assertEqual(sorted(archive.getnames()), ["README.txt", "manifest.json", "sources/source.tar.xz"])
            members = {member.name: member for member in archive.getmembers()}
            for member in members.values():
                self.assertTrue(member.isfile())
                self.assertEqual((member.mode, member.uid, member.gid, member.mtime), (0o644, 0, 0, 0))
            self.assertEqual(archive.extractfile("sources/source.tar.xz").read(), self.payload)
            self.assertEqual(json.load(archive.extractfile("manifest.json")), self.manifest)
            readme = archive.extractfile("README.txt").read().decode()
            head = subprocess.check_output(["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip()
            self.assertIn(head, readme)
            self.assertIn("tests/macos-ci.sh", readme)

    def test_source_version_matches_bundled_qt(self) -> None:
        matching = self.run_tool("--check", "--qt-version", "6.10.2")
        self.assertEqual(matching.returncode, 0, matching.stderr)
        changed = self.run_tool("--check", "--qt-version", "6.11.0")
        self.assertEqual(changed.returncode, 2)
        self.assertIn("does not match", changed.stderr)

    def test_build_only_project_uses_an_already_pinned_source(self) -> None:
        path = self.root / "CMakeLists.txt"
        pins = path.read_text(encoding="utf-8")
        project = ('ExternalProject_Add(source_build SOURCE_DIR "${x_SOURCE_DIR}" '
                   'DOWNLOAD_COMMAND "" UPDATE_COMMAND "" BUILD_IN_SOURCE TRUE '
                   'CONFIGURE_COMMAND "${x_SOURCE_DIR}/configure" BUILD_COMMAND make)\n')
        path.write_text(pins + project, encoding="utf-8")
        result = self.run_tool("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        for invalid in (
            project.replace("x_SOURCE_DIR", "unknown_SOURCE_DIR"),
            project.replace('DOWNLOAD_COMMAND ""', 'DOWNLOAD_COMMAND fetch'),
            project.replace('UPDATE_COMMAND ""', 'UPDATE_COMMAND update'),
            project.replace('DOWNLOAD_COMMAND ""', ''),
            project.replace('UPDATE_COMMAND ""', ''),
            project.replace('BUILD_IN_SOURCE TRUE', 'GIT_REPOSITORY https://example.invalid/unpinned.git'),
            project.replace('BUILD_IN_SOURCE TRUE', 'DOWNLOAD_COMMAND fetch'),
            project.replace('BUILD_IN_SOURCE TRUE', 'SOURCE_DIR unverified'),
        ):
            with self.subTest(declaration=invalid):
                path.write_text(pins + invalid, encoding="utf-8")
                result = self.run_tool("--check")
                self.assertEqual(result.returncode, 2)
                self.assertIn("unhashed CMake", result.stderr)

    def test_corrupt_cache_preserves_existing_output(self) -> None:
        cache = self.root / "cache"
        cache.mkdir()
        (cache / "source.tar.xz").write_bytes(b"corrupt")
        output = self.root / "existing.tar.gz"
        output.write_bytes(b"keep")
        result = self.run_tool("--cache", str(cache), "--output", str(output))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b"keep")
        self.assertFalse(any(output.parent.glob(f".{output.name}.*")))

    def test_changed_or_unhashed_build_pins_are_rejected(self) -> None:
        path = self.root / "CMakeLists.txt"
        for declaration, message in (
            (f"FetchContent_Declare(x URL {self.url} URL_HASH SHA256={'0' * 64})", "does not cover"),
            (f"FetchContent_Declare(x URL {self.url})", "unhashed CMake"),
        ):
            with self.subTest(declaration=declaration):
                path.write_text(declaration, encoding="utf-8")
                result = self.run_tool("--check")
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
        for relative in ("CMakeLists.txt", "cmake/AudioProcessing.cmake", "cmake/Updates.cmake", "tests/macos-ci.sh", "tests/windows-ci.ps1"):
            (self.root / relative).write_text("# no pins\n", encoding="utf-8")
        self.assertIn("no pinned source archives", self.run_tool("--check").stderr)

    def test_output_inside_cache_is_rejected(self) -> None:
        cache = self.root / "cache"
        cache.mkdir()
        (cache / "source.tar.xz").write_bytes(self.payload)
        result = self.run_tool("--cache", str(cache), "--output", str(cache / "out.tar.gz"))
        self.assertNotEqual(result.returncode, 0)

    def test_manifest_rejects_duplicate_bad_url_and_bad_json(self) -> None:
        duplicate = dict(self.manifest)
        duplicate["archives"] = duplicate["archives"] * 2
        (self.root / "cmake/source_archives.json").write_text(json.dumps(duplicate), encoding="utf-8")
        self.assertNotEqual(self.run_tool("--check").returncode, 0)
        for overrides in ({"url": "http://example.invalid/source.tar.xz"}, {"url": "https://"},
                          {"url": "https://example.invalid:invalid/file"}, {"name": "../unsafe.tar"},
                          {"sha256": "xyz"}, {"binary_url": self.url}):
            with self.subTest(overrides=overrides):
                duplicate["archives"] = [dict(self.manifest["archives"][0], **overrides)]
                (self.root / "cmake/source_archives.json").write_text(json.dumps(duplicate), encoding="utf-8")
                result = self.run_tool("--check")
                self.assertEqual(result.returncode, 2)
                if "url" in overrides:
                    self.assertIn("HTTPS URL", result.stderr)
        (self.root / "cmake/source_archives.json").write_bytes(b"{\xff")
        self.assertNotEqual(self.run_tool("--check").returncode, 0)

    def test_missing_manifest_pin_and_required_arguments_fail(self) -> None:
        (self.root / "cmake/Updates.cmake").unlink()
        self.assertNotEqual(self.run_tool("--check").returncode, 0)
        (self.root / "cmake/Updates.cmake").write_text("# restored\n", encoding="utf-8")
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertNotEqual(self.run_tool("--check", "--output", str(self.root / "x")).returncode, 0)

    def test_missing_origin_and_cached_symlink_fail_without_output(self) -> None:
        subprocess.run(["git", "-C", str(self.root), "config", "--unset", "remote.origin.url"], check=True)
        cache = self.root / "cache"
        cache.mkdir()
        source = cache / "source.tar.xz"
        source.write_bytes(self.payload)
        output = self.root / "out.tar.gz"
        self.assertNotEqual(self.run_tool("--cache", str(cache), "--output", str(output)).returncode, 0)
        self.assertFalse(output.exists())
        source.unlink()
        try:
            source.symlink_to(self.root / "real-source.tar.xz")
        except OSError as error:
            if getattr(error, "winerror", None) != 1314:
                raise
            self.skipTest("Windows account has no symbolic-link privilege")
        (self.root / "real-source.tar.xz").write_bytes(self.payload)
        subprocess.run(["git", "-C", str(self.root), "config", "remote.origin.url", "https://example.invalid/repo.git"], check=True)
        self.assertNotEqual(self.run_tool("--cache", str(cache), "--output", str(output)).returncode, 0)
        self.assertFalse(output.exists())

    def test_https_downloads_are_verified_before_publication(self) -> None:
        certificate = self.root / "certificate.pem"
        key = self.root / "key.pem"
        config = self.root / "certificate.conf"
        config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=extensions\nprompt=no\n"
                          "[dn]\nCN=localhost\n[extensions]\nsubjectAltName=IP:127.0.0.1\n"
                          "basicConstraints=critical,CA:TRUE\n", encoding="utf-8")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-config", str(config), "-keyout", str(key), "-out", str(certificate)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        payload = self.payload
        plain_requests = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                if not isinstance(self.connection, ssl.SSLSocket):
                    plain_requests.append(self.path)
                if self.path in ("/redirect", "/secure-redirect"):
                    self.send_response(302)
                    target = f"http://127.0.0.1:{plain.server_port}" if self.path == "/redirect" else f"https://127.0.0.1:{secure.server_port}"
                    self.send_header("Location", target + "/valid")
                    self.end_headers()
                    return
                body = b"wrong content" if self.path == "/corrupt" else payload
                self.send_response(404 if self.path == "/missing" else 200)
                self.send_header("Content-Length", str(512 * 1024 * 1024 + 1 if self.path == "/large" else len(body)))
                self.end_headers()
                if self.path != "/large":
                    self.wfile.write(body)
        plain = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        secure = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        for server in (plain, secure):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.addCleanup(thread.join, 5)
            self.addCleanup(server.server_close)
            self.addCleanup(server.shutdown)
        environment = dict(os.environ, SSL_CERT_FILE=str(certificate), NO_PROXY="127.0.0.1,localhost")
        for route, success in (("valid", True), ("secure-redirect", True), ("corrupt", False), ("missing", False), ("large", False), ("redirect", False)):
            with self.subTest(route=route):
                url = f"https://127.0.0.1:{secure.server_port}/{route}"
                for relative in ("CMakeLists.txt", "cmake/AudioProcessing.cmake", "cmake/Updates.cmake", "tests/macos-ci.sh", "tests/windows-ci.ps1"):
                    path = self.root / relative
                    path.write_text(path.read_text().replace(self.url, url), encoding="utf-8")
                self.url = url
                self.manifest["archives"][0]["url"] = url
                (self.root / "cmake/source_archives.json").write_text(json.dumps(self.manifest), encoding="utf-8")
                cache = self.root / (route + "-cache")
                output = self.root / (route + ".tar.gz")
                output.write_bytes(b"previous package")
                result = self.run_tool("--cache", str(cache), "--output", str(output), environment=environment)
                self.assertEqual(result.returncode == 0, success, result.stderr)
                if success:
                    self.assertEqual((cache / "source.tar.xz").read_bytes(), payload)
                    with tarfile.open(output) as archive:
                        self.assertEqual(archive.extractfile("sources/source.tar.xz").read(), payload)
                else:
                    self.assertEqual(output.read_bytes(), b"previous package")
                    self.assertFalse((cache / "source.tar.xz").exists())
                self.assertFalse(list(cache.glob(".*")))
        self.assertEqual(plain_requests, [], "HTTPS redirects must be rejected before any plaintext request")


if __name__ == "__main__":
    unittest.main()
