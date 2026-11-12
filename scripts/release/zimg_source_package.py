#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse a staged libzimg whose source package the licence record does not name.

Issue #3396: `release-native` ships the `zimg` package that `dnf zimg-devel`
resolves in the `release-build` container, while
`tools/rc1-tester/image/licensing.json` credits one measured source package.
`dev/Containerfile` pins no version, so an EPEL rebuild would ship a different
library with nothing to notice: the notice would keep naming the old source.
`stage_zimg` runs this check before it copies the library, and the record test
reads the same `zimg-source-rpm.txt`, so the expected value has one home.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

RECORD = Path("tools/rc1-tester/image/zimg-source-rpm.txt")


def recorded_source_rpm(record: Path = RECORD) -> str:
    """The source package the record names, e.g. ``zimg-3.0.6-1.el8.src.rpm``."""
    value = record.read_text(encoding="utf-8").strip()
    if not value:
        raise ValueError(f"{record} records no source package")
    return value


def source_rpm_of(library: Path, *, runner=subprocess.run) -> str:
    """The ``SOURCERPM`` of the installed package that owns ``library``."""
    result = runner(
        ["rpm", "-qf", "--qf", "%{SOURCERPM}", str(library)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise ValueError(f"cannot read the source package of {library}: {detail}")
    value = result.stdout.strip()
    if not value:
        raise ValueError(f"the package that owns {library} records no source package")
    return value


def check_source_package(
    library: Path, record: Path = RECORD, *, runner=subprocess.run
) -> str:
    """Return the measured source package, or raise when it is not the recorded one."""
    expected = recorded_source_rpm(record)
    actual = source_rpm_of(library, runner=runner)
    if actual != expected:
        raise ValueError(
            f"{library} comes from {actual}, but the licence record names {expected}"
        )
    return actual


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path, help="staged libzimg file")
    parser.add_argument("--record", type=Path, default=RECORD, help="recorded source package")
    args = parser.parse_args(argv)
    try:
        check_source_package(args.library, args.record)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
