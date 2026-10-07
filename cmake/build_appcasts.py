#!/usr/bin/env python3
"""Build architecture-specific Sparkle appcasts from signed archives."""

from __future__ import annotations

import argparse
import base64
import pathlib
import plistlib
import re
import sys
import xml.etree.ElementTree as ET
import zipfile


SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
ET.register_namespace("sparkle", SPARKLE)


def signature(value: str) -> str:
    value = value.strip()
    try:
        decoded = base64.b64decode(value, validate=True)
    except ValueError as error:
        raise ValueError(f"invalid Ed25519 signature: {error}")
    if len(decoded) != 64:
        raise ValueError("Ed25519 signatures must decode to 64 bytes")
    return value


def public_key(value: str) -> str:
    value = value.strip()
    try:
        decoded = base64.b64decode(value, validate=True)
    except ValueError as error:
        raise ValueError(f"invalid Sparkle public key: {error}")
    if len(decoded) != 32:
        raise ValueError("Sparkle public keys must decode to 32 bytes")
    return value


def verify_archive(archive: pathlib.Path, expected_key: str, version: str):
    try:
        with zipfile.ZipFile(archive) as bundle:
            metadata = plistlib.loads(bundle.read("squadspeak.app/Contents/Info.plist"))
    except (OSError, zipfile.BadZipFile, KeyError, plistlib.InvalidFileException) as error:
        raise ValueError(f"cannot inspect app metadata in {archive}: {error}")
    for field, expected in (("CFBundleIdentifier", "app.squadspeak.desktop"), ("CFBundleVersion", version),
                            ("CFBundleShortVersionString", version)):
        if not isinstance(metadata, dict) or metadata.get(field) != expected:
            raise ValueError(f"archive {field} does not match the release: {archive}")
    embedded = metadata.get("SUPublicEDKey")
    if not isinstance(embedded, str) or public_key(embedded) != expected_key:
        raise ValueError(f"archive SUPublicEDKey does not match signing key: {archive}")


def make_feed(version: str, repository: str, architecture: str,
              archive: pathlib.Path, archive_signature: str,
              expected_public_key: str) -> ET.ElementTree:
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version):
        raise ValueError("version must be YYYY.M.D-style numeric components")
    if architecture not in {"arm64", "x86_64"}:
        raise ValueError(f"unsupported macOS architecture: {architecture}")
    if repository != "YunaBraska/SquadSpeak":
        raise ValueError("repository must be YunaBraska/SquadSpeak")
    expected_name = f"squadspeak-macos-{architecture}.zip"
    if archive.name != expected_name:
        raise ValueError(f"archive name must be {expected_name}")
    if not archive.is_file() or archive.stat().st_size <= 0:
        raise ValueError(f"archive is missing or empty: {archive}")
    verify_archive(archive, expected_public_key, version)
    signature_value = signature(archive_signature)
    root = ET.Element("rss", {"version": "2.0"})
    channel = ET.SubElement(root, "channel")
    ET.SubElement(channel, "title").text = "SquadSpeak"
    ET.SubElement(channel, "link").text = f"https://github.com/{repository}/releases"
    ET.SubElement(channel, "description").text = "SquadSpeak macOS updates"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = f"SquadSpeak {version}"
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}minimumSystemVersion").text = "13.0"
    ET.SubElement(item, "enclosure", {
        "url": f"https://github.com/{repository}/releases/download/{version}/{archive.name}",
        "length": str(archive.stat().st_size),
        "type": "application/octet-stream",
        f"{{{SPARKLE}}}edSignature": signature_value,
    })
    return ET.ElementTree(root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    parser.add_argument("--arm-archive", required=True, type=pathlib.Path)
    parser.add_argument("--arm-signature", required=True)
    parser.add_argument("--x86-archive", required=True, type=pathlib.Path)
    parser.add_argument("--x86-signature", required=True)
    parser.add_argument("--public-key", required=True)
    args = parser.parse_args()
    expected_public_key = public_key(args.public_key)
    feeds = []
    for architecture, archive, archive_signature in (
        ("arm64", args.arm_archive, args.arm_signature),
        ("x86_64", args.x86_archive, args.x86_signature),
    ):
        feed = make_feed(args.version, args.repository, architecture, archive, archive_signature,
                          expected_public_key)
        feeds.append((args.output_dir / f"appcast-macos-{architecture}.xml", feed))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for output, feed in feeds:
        feed.write(output, encoding="utf-8", xml_declaration=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as error:
        print(f"build_appcasts: {error}", file=sys.stderr)
        sys.exit(2)
