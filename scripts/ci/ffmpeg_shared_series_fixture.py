"""Build a stand-in shared FFmpeg fix series for the tests (ADR-3143).

The real series is a release tarball of VMAFx/ffmpeg-patches that
``scripts/ci/ffmpeg-shared-series.sh`` downloads and verifies. A test gives the
script a tarball of the same layout through a ``file://`` URL and pins its
sha256 in the fixture's ``build-config.env``, so the script's own checks run.
"""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import hashlib
import io
import tarfile
from pathlib import Path

TAG = "v0.0.0-fixture"
PATCH_NAME = "0001-shared-fix.patch"


def tarball(
    directory: Path,
    base_tag: str,
    base_commit: str,
    patches: dict[str, str],
    *,
    extra_members: dict[str, bytes] | None = None,
) -> tuple[Path, str]:
    """Write ``ffmpeg-patches-<TAG>.tar.gz`` into *directory*; return it and its sha256."""
    root = f"ffmpeg-patches-{TAG}"
    members = {
        f"{root}/base.env": (
            f"FFMPEG_REMOTE=https://example.invalid/ffmpeg.git\n"
            f"FFMPEG_TAG={base_tag}\nFFMPEG_COMMIT={base_commit}\n"
        ).encode(),
        f"{root}/series.txt": ("# fixture\n" + "".join(f"{name}\n" for name in patches)).encode(),
    }
    for name, text in patches.items():
        members[f"{root}/patches/{name}"] = text.encode()
    members.update(extra_members or {})
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"ffmpeg-patches-{TAG}.tar.gz"
    with tarfile.open(path, "w:gz") as archive:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def pins(path: Path, sha256: str) -> str:
    """The ``build-config.env`` lines that pin the stand-in series."""
    return (
        'FFMPEG_FIX_SERIES_REPO="https://example.invalid/fixture/ffmpeg-patches"\n'
        f'FFMPEG_FIX_SERIES_TAG="{TAG}"\n'
        f'FFMPEG_FIX_SERIES_SHA256="{sha256}"\n'
        f'FFMPEG_FIX_SERIES_URL="file://{path}"\n'
    )
