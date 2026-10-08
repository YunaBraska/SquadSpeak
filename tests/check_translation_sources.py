"""Compare Qt's current source extraction with every shipped catalog."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def messages(path):
    return {(context.findtext("name"), message.findtext("source"), message.findtext("comment", ""))
            for context in ET.parse(path).findall("context")
            for message in context.findall("message")
            if message.find("translation") is None
            or message.find("translation").get("type") not in ("obsolete", "vanished")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lupdate")
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="squadspeak-translations-") as directory:
        extracted = Path(directory) / "sources.ts"
        subprocess.run([args.lupdate, str(args.source / "src"), str(args.source / "ui"),
                        "-no-obsolete", "-locations", "none", "-ts", str(extracted)], check=True,
                       stdout=subprocess.DEVNULL)
        expected = messages(extracted)
        catalogs = sorted((args.source / "ui/i18n").glob("squadspeak_*.ts"))
        if not expected or not catalogs:
            raise SystemExit("Source extraction or translation catalogs are empty.")
        failures = []
        for catalog in catalogs:
            actual = messages(catalog)
            for context, source, comment in sorted(expected - actual):
                failures.append(f"{catalog.name}: missing {context}: {source} ({comment})")
            for context, source, comment in sorted(actual - expected):
                failures.append(f"{catalog.name}: unused {context}: {source} ({comment})")
            for context in ET.parse(catalog).findall("context"):
                if context.findtext("name") != "Headless":
                    continue
                for message in context.findall("message"):
                    source = message.findtext("source", "")
                    translated = "".join(message.find("translation").itertext())
                    tokens = re.findall(r"--[a-z]+(?:-[a-z]+)*|SQUADSPEAK_[A-Z_]*|(?:config/)?application\.properties|key=value|confirmed=true|\b(?:passwordFile|settingsFile|identityFile|channelId)\b", source)
                    for prefix, names in {
                        "Optional own channel ID": "status configure password admission history chat radio",
                        "Supporter action must": "status sign-in refresh sign-out cancel",
                        "Channel action must": "list add remove",
                        "search requires": "search query offset limit",
                        "add creates": "add id update",
                        "Unknown command.": "help",
                    }.items():
                        if source.startswith(prefix):
                            tokens.extend(names.split())
                    for name in ("id", "text", "values", "value"):
                        if source.startswith(name + " "):
                            tokens.append(name)
                    for token in set(tokens):
                        if not re.search(r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])", translated):
                            failures.append(f"{catalog.name}: changed CLI token {token}: {source}")
                    if re.search(r"\\u[0-9a-fA-F]{4}", translated):
                        failures.append(f"{catalog.name}: escaped Unicode in visible text: {source}")
            code = catalog.stem.removeprefix("squadspeak_")
            permissions = args.source / "ui/i18n" / f"{code}.lproj/InfoPlist.strings"
            if not permissions.is_file():
                failures.append(f"{permissions.name}: missing {code} microphone/network permission translations")
                continue
            try:
                entries = dict((key, json.loads(value)) for key, value in re.findall(
                    r'^"([A-Za-z]+)"\s*=\s*("(?:[^"\\]|\\.)*");$', permissions.read_text(encoding="utf-8"), re.MULTILINE))
                for key in ("NSMicrophoneUsageDescription", "NSLocalNetworkUsageDescription", "NSAudioCaptureUsageDescription"):
                    if not entries.get(key, "").strip():
                        failures.append(f"{permissions}: missing {key}")
            except (ValueError, UnicodeError) as error:
                failures.append(f"{permissions}: {error}")
        if failures:
            raise SystemExit("\n".join(failures))
        print(f"{len(expected)} source messages covered by all {len(catalogs)} catalogs.")


if __name__ == "__main__":
    main()
