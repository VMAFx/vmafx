#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fetch a small subset of the YouTube UGC dataset for tiny-AI corpus
expansion (T6-x — vmaf_tiny_v5 candidate).

Downloads the N smallest 4-tuple ``(orig, cbr, vod, vodlb)`` stems from
``gs://ugc-dataset/vp9_compressed_videos/``. Each tuple yields three
``(orig, dis)`` ref/dis pairs (orig→cbr, orig→vod, orig→vodlb), so
N stems = 3*N training pairs. Picks the smallest by total compressed
size to keep wall-time + disk reasonable for a corpus-expansion probe.

YouTube UGC license: Creative Commons Attribution (per ATTRIBUTION
file at the bucket root). See docs/ai/training-data.md for the
combined-corpus license register.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

REPO_ROOT = Path(__file__).resolve().parents[2]
AI_SRC = REPO_ROOT / "ai" / "src"

if str(AI_SRC) not in sys.path:
    sys.path.insert(0, str(AI_SRC))

from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

# urlopen applies this per socket operation, so it bounds a stall, not a whole
# download. Not measured: it depends on the network.
API_TIMEOUT_S = 60

GCS_LIST_URL = "https://storage.googleapis.com/storage/v1/b/ugc-dataset/o"
GCS_OBJ_URL = "https://storage.googleapis.com/ugc-dataset/{name}"
PREFIX = "vp9_compressed_videos/"
SUFFIXES = ("orig", "cbr", "vod", "vodlb")


def _validate_https_url(url: str) -> None:
    """Reject non-HTTPS schemes, non-GCS hostnames, credentials, or non-standard ports."""
    parsed = urlsplit(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname != "storage.googleapis.com"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port not in {None, 443}
    ):
        raise ValueError(f"refusing non-GCS HTTPS URL: {url!r}")


class _HttpsOnlyRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep urllib redirects inside the same validated HTTPS trust boundary."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> urllib.request.Request | None:
        _validate_https_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _opener(url: str) -> urllib.request.OpenerDirector:
    """Build an opener for url enforcing GCS HTTPS-only redirect validation."""
    _validate_https_url(url)
    return urllib.request.build_opener(_HttpsOnlyRedirectHandler())


def _list_bucket(prefix: str) -> list[dict]:
    """List items in the GCS bucket matching the specified prefix."""
    url = f"{GCS_LIST_URL}?prefix={prefix}&maxResults=5000&fields=items(name,size)"
    with _opener(url).open(url, timeout=API_TIMEOUT_S) as resp:
        data = json.loads(resp.read().decode())
    return data.get("items", [])


def _group_by_stem(items: list[dict]) -> dict[str, list[tuple[str, int]]]:
    groups: dict[str, list[tuple[str, int]]] = {}
    for it in items:
        n = it["name"].split("/")[-1]
        if not n:
            continue
        # Files look like Gaming_720P-25aa_orig.mp4 / _cbr.webm / _vod.webm / _vodlb.webm
        # Find which suffix prefixes the extension.
        stem = None
        for sfx in SUFFIXES:
            for ext in (".mp4", ".webm"):
                tag = f"_{sfx}{ext}"
                if n.endswith(tag):
                    stem = n[: -len(tag)]
                    break
            if stem is not None:
                break
        if stem is None:
            continue
        groups.setdefault(stem, []).append((n, int(it["size"])))
    # only complete 4-tuples
    return {s: f for s, f in groups.items() if len(f) == 4}


def _download(url: str, dest: Path) -> None:
    """Download a remote file from the given URL to the destination path."""
    if dest.exists() and dest.stat().st_size > 0:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  downloading {url} -> {dest}", flush=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    with _opener(url).open(url, timeout=API_TIMEOUT_S) as r, tmp.open("wb") as f:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            f.write(chunk)
    tmp.rename(dest)


def _suffix_for_name(filename: str) -> str:
    for sfx in SUFFIXES:
        if f"_{sfx}." in filename:
            return sfx
    return "unknown"


def _run_manifest_default(content_manifest: Path) -> Path:
    return content_manifest.with_name(f"{content_manifest.stem}.run-manifest.json")


def _write_run_manifest(
    *,
    args: argparse.Namespace,
    ranked: list[tuple[str, list[tuple[str, int]]]],
    total_bytes: int,
) -> None:
    stems = [
        {
            "stem": stem,
            "files": [
                {
                    "name": filename,
                    "suffix": _suffix_for_name(filename),
                    "size_bytes": size,
                    "path": str(args.out_dir / filename),
                }
                for filename, size in sorted(files)
            ],
        }
        for stem, files in ranked
    ]
    write_manifest_json(
        args.run_manifest_out,
        {
            "schema": "youtube-ugc-subset-fetch-run-manifest-v1",
            "dataset": "youtube-ugc",
            "bucket": "ugc-dataset",
            "prefix": PREFIX,
            "selection": {
                "policy": "smallest-complete-4tuple",
                "n_stems_requested": args.n_stems,
                "stems_selected": len(ranked),
                "files_selected": sum(len(files) for _, files in ranked),
                "total_size_bytes": total_bytes,
            },
            "stems": stems,
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=sys.argv,
                args=args,
                inputs={"bucket_listing": GCS_LIST_URL},
                outputs={
                    "clips_dir": args.out_dir,
                    "content_manifest": args.manifest,
                    "run_manifest": args.run_manifest_out,
                },
            ),
        },
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out-dir", type=Path, required=True, help="Directory to drop downloaded mp4/webm into."
    )
    ap.add_argument(
        "--n-stems", type=int, default=30, help="Pick the N smallest 4-tuple stems. Default 30."
    )
    ap.add_argument(
        "--manifest",
        type=Path,
        required=True,
        help="Output JSON manifest (stem -> {orig,cbr,vod,vodlb}).",
    )
    ap.add_argument(
        "--run-manifest-out",
        type=Path,
        default=None,
        help="Output run-provenance JSON sidecar (default: <manifest>.run-manifest.json).",
    )
    args = ap.parse_args()
    if args.run_manifest_out is None:
        args.run_manifest_out = _run_manifest_default(args.manifest)

    print(f"[ugc-fetch] listing {PREFIX} ...", flush=True)
    items = _list_bucket(PREFIX)
    print(f"[ugc-fetch] {len(items)} items in bucket", flush=True)
    groups = _group_by_stem(items)
    print(f"[ugc-fetch] {len(groups)} complete 4-tuple stems", flush=True)
    ranked = sorted(groups.items(), key=lambda kv: sum(s for _, s in kv[1]))[: args.n_stems]
    total_bytes = sum(sum(s for _, s in files) for _, files in ranked)
    total_mb = total_bytes / 1e6
    print(f"[ugc-fetch] selected {len(ranked)} stems totalling {total_mb:.1f} MB", flush=True)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, dict[str, str]] = {}
    for i, (stem, files) in enumerate(ranked, 1):
        print(f"[ugc-fetch] [{i}/{len(ranked)}] stem={stem}", flush=True)
        entry: dict[str, str] = {}
        for fname, _sz in files:
            url = GCS_OBJ_URL.format(name=PREFIX + fname)
            dest = args.out_dir / fname
            _download(url, dest)
            entry[_suffix_for_name(fname)] = str(dest)
        manifest[stem] = entry

    write_manifest_json(args.manifest, manifest)
    _write_run_manifest(args=args, ranked=ranked, total_bytes=total_bytes)
    print(
        f"[ugc-fetch] wrote {args.manifest} ({len(manifest)} stems); "
        f"run manifest {args.run_manifest_out}",
        flush=True,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
