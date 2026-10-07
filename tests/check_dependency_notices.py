"""Check bundled Qt module provenance and the accompanying license texts."""
import argparse
from pathlib import Path
import re


def check_notices(directory, modules=()):
    sbom = directory / "QT-SBOM"
    manifest = sbom / "SPDX-LICENSE-IDS.txt"
    if not manifest.is_file():
        return ["Missing SPDX license manifest."]
    declared = {line for line in manifest.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#")}
    errors = []
    covered = set()
    required = set()
    for record in sorted(sbom.glob("*.spdx")):
        text = record.read_text(encoding="utf-8")
        covered.update(re.findall(r"^FileName: \./lib/Qt([^/.]+)\.framework/", text, re.MULTILINE))
        covered.update(re.findall(r"^FileName: \./bin/Qt6([^/.]+)\.dll$", text, re.MULTILINE))
        for reference in set(re.findall(r"LicenseRef-[A-Za-z0-9.+-]+", text)):
            if not re.search(r"^LicenseID: " + re.escape(reference) + r"\nExtractedText: <text>\S", text, re.MULTILINE):
                errors.append(f"Missing embedded license text in {record.name}: {reference}")
        for package in text.split("\nPackageName:")[1:]:
            purpose = re.search(r"^PrimaryPackagePurpose: (.+)$", package, re.MULTILINE)
            if purpose and purpose.group(1) != "LIBRARY":
                continue
            for expression in re.findall(r"^PackageLicense(?:Concluded|Declared): (.+)$", package, re.MULTILINE):
                required.update(token for token in re.findall(r"[A-Za-z0-9.+-]+", expression)
                                if token not in {"NOASSERTION", "NONE", "AND", "OR", "WITH"}
                                and not token.startswith("LicenseRef-"))
    if not covered:
        errors.append("No Qt module SPDX records found.")
    for module in sorted(set(modules) - covered):
        errors.append(f"Missing SPDX record for bundled Qt module: {module}")
    for license_id in sorted(required - declared):
        errors.append(f"License absent from SPDX manifest: {license_id}")
    for license_id in sorted(declared | required):
        path = sbom / "licenses" / (license_id + ".txt")
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            errors.append(f"Missing license text: {license_id}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--qt-modules", nargs="*", default=[])
    args = parser.parse_args()
    errors = check_notices(args.directory, args.qt_modules)
    if errors:
        raise SystemExit("\n".join(errors))
    print("Qt module records and license texts are present.")


if __name__ == "__main__":
    main()
