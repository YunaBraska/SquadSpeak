#!/usr/bin/env python3
"""Rebuild the repository corpus from pinned sources; never run by CTest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request
import wave


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    with path.open("rb") as source:
        checksum = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
        return checksum.hexdigest()


def publish(source, destination):
    temporary = tempfile.NamedTemporaryFile(dir=destination.parent, delete=False)
    pending = Path(temporary.name)
    try:
        with temporary, source.open("rb") as data:
            shutil.copyfileobj(data, temporary)
        pending.replace(destination)
    finally:
        pending.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--refresh-fixture-hashes", action="store_true",
                        help="Explicitly accept regenerated PCM hashes after review")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("version") != 2 or manifest.get("sample_rate") != 48000:
        raise ValueError("Expected corpus manifest version 2 at 48000 Hz")
    identifiers = set()
    for item in manifest["sources"]:
        ident = item["id"]
        if not isinstance(ident, str) or not re.fullmatch(r"[a-z0-9-]+", ident) or ident in identifiers:
            raise ValueError("Invalid or duplicate fixture ID")
        identifiers.add(ident)
        if (type(item["start_sample"]) is not int or item["start_sample"] < 0
                or type(item["frames"]) is not int or not 0 < item["frames"] <= 12 * 48000):
            raise ValueError("Invalid excerpt bounds: " + ident)
    args.cache.mkdir(parents=True, exist_ok=True)
    (root / "licenses").mkdir(exist_ok=True)
    staged = []
    verified = set()
    for item in manifest["sources"]:
        for key in ("audio", "license"):
            source = item[key]
            name = (Path(urllib.parse.urlparse(source["url"]).path).name
                    if "archive_member" in source else item["id"] + (".source" if key == "audio" else ".txt"))
            path = args.cache / name
            if not path.exists():
                temporary = tempfile.NamedTemporaryFile(dir=args.cache, delete=False)
                pending = Path(temporary.name)
                try:
                    with temporary:
                        if "notice" in source:
                            temporary.write(source["notice"].encode("utf-8"))
                        else:
                            request = urllib.request.Request(source["url"], headers={"User-Agent": "SquadSpeak-audio-corpus/1.0"})
                            with urllib.request.urlopen(request, timeout=60) as response:
                                shutil.copyfileobj(response, temporary)
                        temporary.flush()
                    if file_digest(pending) != source["sha256"]:
                        raise ValueError("Source checksum mismatch: " + source["url"])
                    pending.replace(path)
                finally:
                    pending.unlink(missing_ok=True)
            if (path, source["sha256"]) not in verified:
                if file_digest(path) != source["sha256"]:
                    raise ValueError("Cache checksum mismatch: " + str(path))
                verified.add((path, source["sha256"]))
            if key == "license":
                staged.append((path, root / "licenses" / (item["id"] + ".txt")))
            elif "archive_member" in source:
                extracted = args.cache / (item["id"] + ".source")
                if not extracted.exists():
                    with tarfile.open(path, "r:gz") as archive:
                        member = archive.getmember(source["archive_member"])
                        if not member.isfile() or not 0 < member.size <= 16 * 1024 * 1024:
                            raise ValueError("Invalid audio archive member: " + member.name)
                        data = archive.extractfile(member).read()
                    if digest(data) != source["member_sha256"]:
                        raise ValueError("Archive member checksum mismatch: " + member.name)
                    extracted.write_bytes(data)
                if file_digest(extracted) != source["member_sha256"]:
                    raise ValueError("Extracted audio checksum mismatch: " + item["id"])
        pcm = subprocess.check_output([
            args.ffmpeg, "-v", "error", "-i", str(args.cache / (item["id"] + ".source")),
            "-ac", "1", "-af", f"aresample=48000,atrim=start_sample={item['start_sample']}:end_sample={item['start_sample'] + item['frames']}",
            "-ar", "48000", "-f", "s16le", "-acodec", "pcm_s16le", "pipe:1"])
        if len(pcm) != 2 * item["frames"]:
            raise ValueError("Unexpected decoded length: " + item["id"])
        path = args.cache / (item["id"] + ".wav")
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(48000)
            output.writeframes(pcm)
        actual = digest(path.read_bytes())
        if not args.refresh_fixture_hashes and actual != item["sha256"]:
            raise ValueError("PCM changed; review decoder/version before accepting: " + item["id"])
        item["sha256"] = actual
        staged.append((path, root / (item["id"] + ".wav")))
        print(item["id"], actual)
    # Validate every source and conversion before touching the repository corpus.
    for source, destination in staged:
        publish(source, destination)
    if args.refresh_fixture_hashes:
        manifest["decoder"] = subprocess.check_output([args.ffmpeg, "-version"], text=True).splitlines()[0]
        updated = args.cache / "manifest.json"
        updated.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        publish(updated, manifest_path)


if __name__ == "__main__":
    main()
