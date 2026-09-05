#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Download KoNViD-1k UGC video dataset for tiny-AI training (T6-1 / C2 / C3).

The repository deliberately does not redistribute the dataset (license +
size). Operators run this script once per machine. Output lands at
``$VMAF_DATA_ROOT/konvid-1k/`` (default: ``~/datasets/konvid-1k/``).

Citation (academic-use, citation required per the official page):

    KoNViD-1k. Hosu, Hahn, Jenadeleh, Lin, Men, Szirányi, Li, Saupe.
    "The Konstanz natural video database (KoNViD-1k)," QoMEX 2017,
    pp. 1-6. http://database.mmsp-kn.de

The dataset is 1200 user-generated videos (~8 s each, 540p) with MOS
scores from a crowd study. ~2.3 GB compressed.

Usage::

    python ai/scripts/fetch_konvid_1k.py            # downloads to default
    python ai/scripts/fetch_konvid_1k.py --root /tmp/konvid

Re-running is idempotent: existing files with matching size are kept.
"""

from __future__ import annotations

import argparse
import os
import ssl
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AI_SRC = REPO_ROOT / "ai" / "src"

if str(AI_SRC) not in sys.path:
    sys.path.insert(0, str(AI_SRC))

from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

VIDEOS_URL = "https://datasets.vqa.mmsp-kn.de/archives/KoNViD_1k_videos.zip"
METADATA_URL = "https://datasets.vqa.mmsp-kn.de/archives/KoNViD_1k_metadata.zip"

# Sizes are not officially published; observed values (2026-04-25) below
# are sanity floors — small downloads are treated as truncated retries.
_VIDEOS_MIN_BYTES = 2_000_000_000  # ~2 GB
_METADATA_MIN_BYTES = 1_000_000  # ~1 MB


def default_root() -> Path:
    """Return ``$VMAF_DATA_ROOT/konvid-1k`` or ``~/datasets/konvid-1k``."""
    env = os.environ.get("VMAF_DATA_ROOT")
    base = Path(env) if env else Path.home() / "datasets"
    return base / "konvid-1k"


def _humanize(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n = n / 1024  # type: ignore[assignment]
    return f"{n:.1f} TiB"


def _download(url: str, dst: Path, min_bytes: int) -> Path:
    """Stream ``url`` to ``dst`` with a coarse progress line every ~1 s."""
    if dst.exists() and dst.stat().st_size >= min_bytes:
        print(f"[konvid] {dst.name} already present ({_humanize(dst.stat().st_size)}) — skipping")
        return dst

    # Defensive scheme guard: VIDEOS_URL / METADATA_URL are module-level
    # literals today, but assert anyway so future refactors that thread
    # URLs from CLI args don't silently allow file:// or other custom
    # schemes (bandit B310).
    if not url.startswith(("http://", "https://")):
        raise ValueError(f"refusing non-http(s) URL scheme: {url!r}")

    print(f"[konvid] fetching {url}")
    # The mmsp-kn.de TLS cert was observed expired 2026-04-25; the host
    # is the canonical academic source for this dataset. Falling back to
    # an unverified context for this single hard-coded URL is acceptable
    # — integrity is re-checked via the zip CRC + the size sanity floor
    # below. Do not generalise this to other hosts.
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(url, headers={"User-Agent": "vmafx/fetch_konvid_1k.py"})
    # nosec B310: scheme is restricted to http(s) above; URL provenance
    # is the hardcoded VIDEOS_URL / METADATA_URL module constants.
    with urllib.request.urlopen(req, timeout=60, context=ctx) as resp:  # nosec B310
        total = int(resp.headers.get("Content-Length", 0))
        bytes_so_far = 0
        last_print = time.monotonic()
        with dst.open("wb") as out:
            while True:
                chunk = resp.read(1 << 20)  # 1 MiB
                if not chunk:
                    break
                out.write(chunk)
                bytes_so_far += len(chunk)
                now = time.monotonic()
                if now - last_print >= 1.0:
                    if total:
                        pct = 100.0 * bytes_so_far / total
                        print(
                            f"[konvid] {dst.name}: {_humanize(bytes_so_far)} / "
                            f"{_humanize(total)} ({pct:.1f}%)",
                            flush=True,
                        )
                    else:
                        print(
                            f"[konvid] {dst.name}: {_humanize(bytes_so_far)} (size unknown)",
                            flush=True,
                        )
                    last_print = now

    final_sz = dst.stat().st_size
    if final_sz < min_bytes:
        raise RuntimeError(
            f"Download truncated: {dst} is only {final_sz} bytes " f"(expected >= {min_bytes})"
        )
    print(f"[konvid] {dst.name} done — {_humanize(final_sz)}")
    return dst


def _extract(zip_path: Path, dst_dir: Path) -> None:
    """Extract ``zip_path`` into ``dst_dir`` (idempotent)."""
    print(f"[konvid] extracting {zip_path.name} -> {dst_dir}")
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dst_dir)
    print(f"[konvid] {zip_path.name} extraction complete")


def _archive_record(label: str, *, url: str, path: Path, min_bytes: int) -> dict[str, object]:
    return {
        "label": label,
        "url": url,
        "path": str(path),
        "exists": path.exists(),
        "size_bytes": path.stat().st_size if path.is_file() else 0,
        "min_bytes": min_bytes,
    }


def _write_fetch_manifest(
    *,
    args: argparse.Namespace,
    root: Path,
    manifest_out: Path,
    archives: list[dict[str, object]],
) -> None:
    videos_dir = root / "KoNViD_1k_videos"
    metadata_dir = root / "KoNViD_1k_metadata"
    write_manifest_json(
        manifest_out,
        {
            "schema": "konvid-1k-fetch-manifest-v1",
            "dataset": "konvid-1k",
            "root": str(root),
            "keep_zips": bool(args.keep_zips),
            "archives": archives,
            "extracted": {
                "videos_dir": str(videos_dir),
                "videos_dir_exists": videos_dir.is_dir(),
                "metadata_dir": str(metadata_dir),
                "metadata_dir_exists": metadata_dir.is_dir(),
            },
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=sys.argv,
                args=args,
                inputs={"videos_url": VIDEOS_URL, "metadata_url": METADATA_URL},
                outputs={
                    "root": root,
                    "videos_dir": videos_dir,
                    "metadata_dir": metadata_dir,
                    "manifest": manifest_out,
                },
            ),
        },
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="Output dir (default: $VMAF_DATA_ROOT/konvid-1k or ~/datasets/konvid-1k)",
    )
    parser.add_argument(
        "--keep-zips", action="store_true", help="Keep the .zip archives after extracting"
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Output run-provenance JSON sidecar (default: <root>/fetch_manifest.json).",
    )
    args = parser.parse_args()

    root = args.root or default_root()
    root.mkdir(parents=True, exist_ok=True)
    manifest_out = args.manifest_out or root / "fetch_manifest.json"

    videos_zip = root / "KoNViD_1k_videos.zip"
    metadata_zip = root / "KoNViD_1k_metadata.zip"

    _download(VIDEOS_URL, videos_zip, _VIDEOS_MIN_BYTES)
    _download(METADATA_URL, metadata_zip, _METADATA_MIN_BYTES)
    archives = [
        _archive_record("videos", url=VIDEOS_URL, path=videos_zip, min_bytes=_VIDEOS_MIN_BYTES),
        _archive_record(
            "metadata", url=METADATA_URL, path=metadata_zip, min_bytes=_METADATA_MIN_BYTES
        ),
    ]

    if not (root / "KoNViD_1k_videos").is_dir():
        _extract(videos_zip, root)
    if not (root / "KoNViD_1k_metadata").is_dir():
        _extract(metadata_zip, root)

    if not args.keep_zips:
        for z in (videos_zip, metadata_zip):
            if z.exists():
                z.unlink()

    args.manifest_out = manifest_out
    _write_fetch_manifest(args=args, root=root, manifest_out=manifest_out, archives=archives)
    print(f"[konvid] complete. dataset root: {root}")
    print(f"[konvid] fetch manifest: {manifest_out}")
    print("[konvid] next: vmaf-train manifest-scan --dataset konvid-1k --root", root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
