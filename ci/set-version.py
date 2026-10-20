#!/usr/bin/env python3
"""Update the source and INF versions before a Cocogitto release commit."""

import datetime
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = (
    ("CMakeLists.txt", re.compile(rb"(?m)^(project\(btrfs VERSION )(\d+\.\d+\.\d+)(\)\r?)$")),
    ("src/btrfs.inf", re.compile(rb"(?m)^(DriverVer\s*=\s*)(\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.0)(\r?)$")),
    ("src/btrfs-vol.inf", re.compile(rb"(?m)^(DriverVer\s*=\s*)(\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.0)(\r?)$")),
)


def update(version: str) -> None:
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("release version must have three numeric components")

    originals = {}
    current_versions = []
    for name, pattern in SOURCES:
        path = ROOT / name
        content = path.read_bytes()
        matches = list(pattern.finditer(content))
        if len(matches) != 1:
            raise ValueError(f"expected one version in {name}, found {len(matches)}")
        value = matches[0].group(2).decode("ascii")
        current_versions.append(value if name == "CMakeLists.txt" else value.split(",")[1].rsplit(".", 1)[0])
        originals[path] = (content, pattern)

    if len(set(current_versions)) != 1:
        raise ValueError("CMake and INF versions differ")
    current = tuple(map(int, current_versions[0].split(".")))
    target = tuple(map(int, version.split(".")))
    if target <= current:
        raise ValueError(f"release version {version} must exceed current {current_versions[0]}")

    date = datetime.datetime.now(datetime.timezone.utc).strftime("%m/%d/%Y")
    for path, (content, pattern) in originals.items():
        value = version if path.name == "CMakeLists.txt" else f"{date},{version}.0"
        updated = pattern.sub(lambda match: match.group(1) + value.encode("ascii") + match.group(3), content)
        path.write_bytes(updated)


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: set-version.py MAJOR.MINOR.PATCH")
        update(sys.argv[1])
    except ValueError as error:
        print(f"set-version: {error}", file=sys.stderr)
        sys.exit(1)
