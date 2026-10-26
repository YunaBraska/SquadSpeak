#!/usr/bin/env python3
"""Package the exact pinned third-party source archives used by the build."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
from urllib.parse import urlsplit, urlunsplit
import urllib.error
import urllib.request


SHA256 = re.compile(r"^[0-9a-f]{64}$")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
MAX_ARCHIVE_SIZE = 512 * 1024 * 1024
PIN_FILES = (
    "CMakeLists.txt",
    "cmake/Dependencies.cmake",
    "cmake/AudioProcessing.cmake",
    "cmake/Updates.cmake",
    "tests/macos-ci.sh",
    "tests/windows-ci.ps1",
)


class SourceError(RuntimeError):
    pass


class HttpsRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        if urlsplit(new_url).scheme.lower() != "https":
            raise SourceError("download redirected away from HTTPS")
        return super().redirect_request(request, response, code, message, headers, new_url)


def _read_manifest(root: Path) -> tuple[dict, bytes]:
    path = root / "cmake" / "source_archives.json"
    try:
        raw = path.read_bytes()
        data = json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SourceError(f"cannot read manifest: {path}: {exc}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("archives"), list):
        raise SourceError("manifest must contain an archives array")
    lock = root / "src/teamspeak_bridge/Cargo.lock"
    if lock.is_file():
        for package in tomllib.loads(lock.read_text(encoding="utf-8"))["package"]:
            source = package.get("source", "")
            if source.startswith("registry+"):
                if source != "registry+https://github.com/rust-lang/crates.io-index":
                    raise SourceError("unsupported Cargo registry")
                name, version = package["name"], package["version"]
                data["archives"].append({"name": f"{name}-{version}.crate",
                    "url": f"https://static.crates.io/crates/{name}/{name}-{version}.crate",
                    "sha256": package["checksum"]})
            elif source.startswith("git+"):
                repository, revision = source[4:].split("#", 1)
                url = repository.split("?", 1)[0] + "/archive/" + revision + ".tar.gz"
                if not any(item["url"] == url for item in data["archives"]):
                    raise SourceError(f"manifest does not cover Cargo git source: {url}")
    return data, (json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode()


def _validate_manifest(data: dict) -> list[dict]:
    if not isinstance(data.get("qt_version"), str) or not VERSION.fullmatch(data["qt_version"]):
        raise SourceError("manifest qt_version must be a semantic version")
    archives = data["archives"]
    seen: set[str] = set()
    for item in archives:
        if not isinstance(item, dict):
            raise SourceError("each archive entry must be an object")
        for key in ("name", "url", "sha256"):
            if not isinstance(item.get(key), str) or not item[key]:
                raise SourceError(f"archive entry is missing {key}")
        name = item["name"]
        if not NAME.fullmatch(name) or name in {".", ".."}:
            raise SourceError(f"unsafe archive name: {name!r}")
        if name in seen:
            raise SourceError(f"duplicate archive name: {name}")
        seen.add(name)
        for url_key, hash_key in (("url", "sha256"), ("binary_url", "binary_sha256")):
            if url_key not in item and hash_key not in item:
                continue
            value = item.get(url_key)
            try:
                parsed = urlsplit(value) if isinstance(value, str) else None
                valid = (parsed is not None and parsed.scheme == "https" and parsed.hostname
                         and not parsed.username and not parsed.password
                         and (parsed.port is None or parsed.port > 0)
                         and not any(character.isspace() for character in value))
            except ValueError:
                valid = False
            if not valid:
                raise SourceError(f"{url_key} must be a valid HTTPS URL for {name}")
            if not isinstance(item.get(hash_key), str) or not SHA256.fullmatch(item[hash_key]):
                raise SourceError(f"invalid {hash_key} for {name}")
    if not archives:
        raise SourceError("manifest contains no archives")
    return archives


def _pins(root: Path) -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    cmake_block = re.compile(r"\b(FetchContent_Declare|ExternalProject_Add)\s*\((.*?)\)", re.S)
    cmake_url = re.compile(r"\bURL\s+(https://[^\s)]+)")
    cmake_hash = re.compile(r"\bURL_HASH\s+SHA256=([0-9a-fA-F]{64})")
    mac_pair = re.compile(r"fetch\s+(\S+)\s+(https://\S+)\s+([0-9a-fA-F]{64})")
    win_pair = re.compile(r'Get-Source\s+"([^"]+)"\s+"(https://[^"]+)"\s+"([0-9a-fA-F]{64})"')
    for relative in PIN_FILES:
        text = (root / relative).read_text(encoding="utf-8")
        if relative.endswith(".cmake") or relative == "CMakeLists.txt":
            blocks = cmake_block.findall(text)
            pinned_sources: set[str] = set()
            for kind, block in blocks:
                urls = cmake_url.findall(block)
                hashes = cmake_hash.findall(block)
                sources = re.findall(r'\bSOURCE_DIR\s+(\S+)', block)
                source = re.fullmatch(r'"\$\{([A-Za-z_]\w*)_SOURCE_DIR\}"', sources[0]) if len(sources) == 1 else None
                if (kind == "ExternalProject_Add" and source and source[1] in pinned_sources
                    and re.findall(r'\bDOWNLOAD_COMMAND\s+(\S+)', block) == ['""']
                    and re.findall(r'\bUPDATE_COMMAND\s+(\S+)', block) == ['""']
                    and not re.search(r'\b(?:URL|URL_HASH|GIT_\w+|SVN_\w+|HG_\w+|CVS_\w+)\s', block)):
                    # This project builds the preceding verified download; it
                    # cannot fetch or update a second, unpinned source.
                    continue
                if len(urls) != 1 or len(hashes) != 1:
                    raise SourceError(f"unsupported or unhashed CMake source declaration in {relative}")
                found.extend((url, digest.lower()) for url, digest in zip(urls, hashes))
                if kind == "FetchContent_Declare":
                    pinned_sources.add(block.split()[0])
        elif relative.endswith("macos-ci.sh"):
            for line in text.splitlines():
                if re.match(r"^\s*fetch\s+", line) and not mac_pair.search(line):
                    raise SourceError(f"unrecognized fetch pin in {relative}: {line.strip()}")
            found.extend((url, digest.lower()) for _, url, digest in mac_pair.findall(text))
        else:
            for line in text.splitlines():
                if re.search(r"(?:^\s*|=\s*)Get-Source\s+", line) and not win_pair.search(line):
                    raise SourceError(f"unrecognized Get-Source pin in {relative}: {line.strip()}")
            found.extend((url, digest.lower()) for _, url, digest in win_pair.findall(text))
    if not found:
        raise SourceError("no pinned source archives found")
    return found


def _validate_pins(root: Path, archives: list[dict]) -> None:
    pairs = {(item["url"], item["sha256"]) for item in archives}
    pairs.update((item["binary_url"], item["binary_sha256"]) for item in archives if "binary_url" in item)
    missing = sorted(set(_pins(root)) - pairs)
    if missing:
        raise SourceError("manifest does not cover pinned archives: " + ", ".join(f"{u} [{h}]" for u, h in missing))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _cached_archive(cache: Path, item: dict) -> Path:
    target = cache / item["name"]
    if target.is_symlink():
        raise SourceError(f"cached archive must not be a symlink: {target}")
    if not target.is_file():
        raise SourceError(f"cached archive is missing: {target}")
    if target.stat().st_size > MAX_ARCHIVE_SIZE:
        raise SourceError(f"cached archive exceeds {MAX_ARCHIVE_SIZE} bytes: {target}")
    actual = _sha256(target)
    if actual != item["sha256"]:
        raise SourceError(f"cached archive hash mismatch: {item['name']} ({actual})")
    return target


def _download(cache: Path, item: dict) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / item["name"]
    if target.exists() or target.is_symlink():
        return _cached_archive(cache, item)
    fd, temporary = tempfile.mkstemp(prefix=f".{item['name']}.", dir=cache)
    os.close(fd)
    temporary_path = Path(temporary)
    published = False
    try:
        opener = urllib.request.build_opener(HttpsRedirectHandler())
        for attempt in range(3):
            try:
                digest = hashlib.sha256()
                with opener.open(item["url"], timeout=60) as response, temporary_path.open("wb") as out:
                    status = getattr(response, "status", None)
                    if status is not None and not 200 <= status < 300:
                        raise SourceError(f"HTTP status {status}")
                    content_length = response.headers.get("Content-Length")
                    if content_length and int(content_length) > MAX_ARCHIVE_SIZE:
                        raise SourceError(f"download exceeds {MAX_ARCHIVE_SIZE} bytes")
                    total = 0
                    while True:
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        total += len(block)
                        if total > MAX_ARCHIVE_SIZE:
                            raise SourceError(f"download exceeds {MAX_ARCHIVE_SIZE} bytes")
                        digest.update(block)
                        out.write(block)
                if digest.hexdigest() != item["sha256"]:
                    raise SourceError(f"downloaded archive hash mismatch: {item['name']}")
                temporary_path.replace(target)
                published = True
                return target
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                if isinstance(exc, urllib.error.HTTPError):
                    exc.close()
                if (attempt == 2
                    or isinstance(exc, urllib.error.HTTPError) and exc.code not in (408, 429, 500, 502, 503, 504)
                    or isinstance(getattr(exc, "reason", None), ssl.SSLError)):
                    raise
                print(f"Retrying {item['name']} after network failure ({attempt + 1}/3): {exc}", file=sys.stderr)
                time.sleep(attempt + 1)
    except (OSError, ValueError, SourceError) as exc:
        raise SourceError(f"could not fetch {item['name']}: {exc}") from exc
    finally:
        if not published:
            temporary_path.unlink(missing_ok=True)


def _git(root: Path, *args: str) -> str:
    try:
        return subprocess.check_output(["git", "-C", str(root), *args], text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SourceError(f"git metadata unavailable: {exc}") from exc


def _public_origin(origin: str) -> str:
    if origin.startswith("git@") and ":" in origin:
        host, path = origin[4:].split(":", 1)
        origin = f"https://{host}/{path}"
    parsed = urlsplit(origin)
    if parsed.scheme in {"http", "https", "ssh"} and parsed.hostname:
        origin = urlunsplit(("https", parsed.hostname, parsed.path, "", ""))
    else:
        raise SourceError("origin must identify a public HTTPS repository")
    return origin.rstrip("/").removesuffix(".git")


def _source_readme(head: str, origin: str) -> bytes:
    public = _public_origin(origin)
    archive = f"{public}/archive/{head}.tar.gz"
    mac_docs = f"{public}/blob/{head}/tests/macos-ci.sh"
    windows_docs = f"{public}/blob/{head}/tests/windows-ci.ps1"
    return ("SquadSpeak pinned third-party source archives\n\n"
            f"Repository: {public}\nRevision: {head}\n"
            f"Project source archive: {archive}\nBuild instructions: {mac_docs}\n"
            f"Windows build instructions: {windows_docs}\n\n"
            "The files in sources/ contain the pinned dependency sources, Qt\n"
            "SDK sources and build patches. Verify their SHA256 in manifest.json.\n"
            "The QtKeychain build corrections are in the repository CMake files.\n"
            "The Abseil Meson build patch is included as a separate ZIP archive.\n"
            "The release workflow contains the remaining\n"
            "platform build instructions.\n").encode()


def _write_archive(output: Path, files: list[tuple[str, bytes | Path]], manifest: bytes) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    os.close(fd)
    temporary_path = Path(temporary)
    try:
        with temporary_path.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0, compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                entries = [("manifest.json", manifest)] + files
                for name, value in sorted(entries, key=lambda pair: pair[0]):
                    if isinstance(value, Path):
                        info = tarfile.TarInfo(name)
                        info.size = value.stat().st_size
                        with value.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        info = tarfile.TarInfo(name)
                        info.size = len(value)
                        archive.addfile(info, io.BytesIO(value))
        temporary_path.replace(output)
    finally:
        temporary_path.unlink(missing_ok=True)


def rust_notices(root: Path, cargo: str) -> bytes:
    manifest = root / "src/teamspeak_bridge/Cargo.toml"
    metadata = json.loads(subprocess.check_output([cargo, "metadata", "--locked", "--format-version", "1",
                                                 "--manifest-path", str(manifest)], cwd=root, text=True))
    lock = manifest.with_name("Cargo.lock")
    texts = {}
    records = ["Rust dependency notices", "Cargo.lock SHA256: " + _sha256(lock), ""]
    for package in sorted(metadata["packages"], key=lambda p: (p["name"], p["version"])):
        if not package.get("source"):
            continue
        directory = Path(package["manifest_path"]).parent
        roots = [directory]
        if package["source"].startswith("git+"):
            roots.extend(parent for parent in directory.parents if parent.name != "checkouts" and "checkouts" in parent.parts)
        files = set()
        for base in roots:
            files.update(path for path in base.iterdir() if path.is_file() and
                         path.name.lower().startswith(("license", "licence", "copying", "notice", "authors")))
            for name in ("licenses", "LICENSES"):
                if (base / name).is_dir():
                    files.update(path for path in (base / name).rglob("*") if path.is_file())
        if package.get("license_file"):
            files.add(directory / package["license_file"])
        # Some published crates put their attribution in the README or source header.
        if not files:
            files.update(path for path in directory.iterdir() if path.is_file() and path.name.lower().startswith("readme"))
            files.update((directory / "src").glob("lib.rs"))
        records.extend([f"Package: {package['name']} {package['version']}", "Source: " + package["source"],
                        "License: " + (package.get("license") or "See accompanying notice")])
        if not files:
            raise SourceError("Missing upstream notices for " + package["name"])
        for path in sorted(files):
            content = path.read_bytes()
            digest = hashlib.sha256(content).hexdigest()
            texts[digest] = content.decode("utf-8")
            records.append("Notice: " + digest)
        records.append("")
    # Rust's statically linked standard library has its own component attribution.
    sysroot = Path(subprocess.check_output([str(Path(cargo).with_name("rustc")) if Path(cargo).is_absolute() else "rustc",
                   "--print", "sysroot"], cwd=root, text=True).strip())
    standard = sysroot / "share/doc/rust"
    records.extend(["Rust standard library", "Source: https://github.com/rust-lang/rust/tree/1.96.0"])
    for path in sorted((standard / "licenses").glob("*")) + [standard / "COPYRIGHT-library.html"]:
        if path.is_file():
            content = path.read_bytes()
            digest = hashlib.sha256(content).hexdigest()
            texts[digest] = content.decode("utf-8")
            records.append("Notice: " + digest)
    for digest, text in sorted(texts.items()):
        records.extend(["", "Notice text: " + digest, text])
    return ("\n".join(records) + "\n").encode()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--rust-notices", action="store_true", help="Write pinned Cargo dependency notices to --output")
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--qt-version", help="Version of the bundled Qt SDK")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        if args.rust_notices:
            if not args.output or args.check or args.cache:
                raise SourceError("--rust-notices requires --output, without --check or --cache")
            args.output.write_bytes(rust_notices(root, args.cargo))
            return 0
        data, manifest = _read_manifest(root)
        archives = _validate_manifest(data)
        if args.qt_version and args.qt_version != data["qt_version"]:
            raise SourceError(f"Qt source version {data['qt_version']} does not match bundled SDK {args.qt_version}")
        _validate_pins(root, archives)
        if args.check:
            if args.cache or args.output:
                raise SourceError("--check cannot be combined with --cache or --output")
            print(f"validated {len(archives)} source archive entries")
            return 0
        if not args.cache or not args.output:
            raise SourceError("--cache and --output are required unless --check is used")
        cache, output = args.cache.resolve(), args.output.resolve()
        if output == cache or cache in output.parents or output in cache.parents:
            raise SourceError("cache and output must not contain each other")
        head = _git(root, "rev-parse", "HEAD")
        origin = _git(root, "config", "--get", "remote.origin.url")
        files = [(f"sources/{item['name']}", _download(cache, item)) for item in archives]
        files.append(("README.txt", _source_readme(head, origin)))
        _write_archive(output, files, manifest)
        print(output)
        return 0
    except SourceError as exc:
        print(f"source archive error: {exc}", file=sys.stderr)
        return 2
    except (OSError, tarfile.TarError) as exc:
        print(f"source archive error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
