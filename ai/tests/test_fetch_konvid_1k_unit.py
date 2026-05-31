# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.fetch_konvid_1k`.

Mocks ``urllib.request.urlopen`` + ``zipfile`` to exercise the
downloader/extractor without ever hitting the network.
"""

from __future__ import annotations

import importlib.util
import io
import json
import zipfile
from pathlib import Path
from unittest.mock import MagicMock

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "fetch_konvid_1k.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("fetch_konvid_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FK = _load_module()


# ---------------------------------------------------------------------------
# default_root
# ---------------------------------------------------------------------------


def test_default_root_uses_env_var(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv("VMAF_DATA_ROOT", str(tmp_path))
    assert FK.default_root() == tmp_path / "konvid-1k"


def test_default_root_falls_back_to_home(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("VMAF_DATA_ROOT", raising=False)
    root = FK.default_root()
    assert root.name == "konvid-1k"
    assert root.parent.name == "datasets"


# ---------------------------------------------------------------------------
# _humanize
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "n,expected_substr",
    [
        (512, "B"),
        (2048, "KiB"),
        (5 * 1024 * 1024, "MiB"),
        (3 * 1024 * 1024 * 1024, "GiB"),
    ],
)
def test_humanize_units(n: int, expected_substr: str) -> None:
    out = FK._humanize(n)
    assert expected_substr in out


def test_humanize_terabyte_branch() -> None:
    enormous = 5 * 1024**4  # 5 TiB
    assert "TiB" in FK._humanize(enormous)


# ---------------------------------------------------------------------------
# _download
# ---------------------------------------------------------------------------


class _FakeResponse:
    """Iterable response that streams ``payload`` in 1-MiB-ish chunks."""

    def __init__(self, payload: bytes) -> None:
        self._stream = io.BytesIO(payload)
        self.headers = {"Content-Length": str(len(payload))}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, n: int) -> bytes:
        return self._stream.read(n)


def test_download_skips_when_already_present(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    dst = tmp_path / "already.zip"
    dst.write_bytes(b"\x00" * 1024)
    result = FK._download("https://example/", dst, min_bytes=100)
    assert result == dst
    out = capsys.readouterr().out
    assert "already present" in out


def test_download_writes_payload(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    dst = tmp_path / "out.bin"
    payload = b"A" * 4096

    fake_urlopen = MagicMock(return_value=_FakeResponse(payload))
    monkeypatch.setattr(FK.urllib.request, "urlopen", fake_urlopen)
    FK._download("https://example/x", dst, min_bytes=100)
    assert dst.read_bytes() == payload


def test_download_truncation_raises(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    dst = tmp_path / "out.bin"
    payload = b"A" * 10  # below 1000-byte sanity floor

    fake_urlopen = MagicMock(return_value=_FakeResponse(payload))
    monkeypatch.setattr(FK.urllib.request, "urlopen", fake_urlopen)
    with pytest.raises(RuntimeError, match="truncated"):
        FK._download("https://example/x", dst, min_bytes=1000)


# ---------------------------------------------------------------------------
# _extract
# ---------------------------------------------------------------------------


def test_extract_unpacks_zip(tmp_path: Path) -> None:
    zip_path = tmp_path / "fixture.zip"
    with zipfile.ZipFile(zip_path, "w") as zf:
        zf.writestr("a.txt", "alpha")
        zf.writestr("sub/b.txt", "beta")
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    FK._extract(zip_path, out_dir)
    assert (out_dir / "a.txt").read_text() == "alpha"
    assert (out_dir / "sub" / "b.txt").read_text() == "beta"


# ---------------------------------------------------------------------------
# _archive_record
# ---------------------------------------------------------------------------


def test_archive_record_existing(tmp_path: Path) -> None:
    path = tmp_path / "x.zip"
    path.write_bytes(b"hello")
    rec = FK._archive_record("videos", url="https://e/", path=path, min_bytes=1)
    assert rec["label"] == "videos"
    assert rec["url"] == "https://e/"
    assert rec["path"] == str(path)
    assert rec["exists"] is True
    assert rec["size_bytes"] == 5
    assert rec["min_bytes"] == 1


def test_archive_record_missing(tmp_path: Path) -> None:
    rec = FK._archive_record("metadata", url="https://e/", path=tmp_path / "absent", min_bytes=1)
    assert rec["exists"] is False
    assert rec["size_bytes"] == 0


# ---------------------------------------------------------------------------
# main entry point
# ---------------------------------------------------------------------------


def test_main_end_to_end_with_mocks(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "konvid"

    # Pre-place both zip stubs that satisfy size sanity (so _download skips).
    root.mkdir(parents=True, exist_ok=True)
    (root / "KoNViD_1k_videos.zip").write_bytes(b"\x00" * (FK._VIDEOS_MIN_BYTES + 1))
    (root / "KoNViD_1k_metadata.zip").write_bytes(b"\x00" * (FK._METADATA_MIN_BYTES + 1))

    extract_calls: list[tuple[Path, Path]] = []

    def fake_extract(zip_path: Path, dst_dir: Path) -> None:
        extract_calls.append((zip_path, dst_dir))
        # Simulate extraction of only the matching sub-dir for this archive.
        if "videos" in zip_path.name:
            (dst_dir / "KoNViD_1k_videos").mkdir(exist_ok=True)
        if "metadata" in zip_path.name:
            (dst_dir / "KoNViD_1k_metadata").mkdir(exist_ok=True)

    monkeypatch.setattr(FK, "_extract", fake_extract)
    monkeypatch.setattr(
        "sys.argv",
        ["fetch_konvid_1k.py", "--root", str(root), "--keep-zips"],
    )

    rc = FK.main()
    assert rc == 0
    # Both archives extract independently.
    assert len(extract_calls) == 2

    manifest = root / "fetch_manifest.json"
    assert manifest.is_file()
    payload = json.loads(manifest.read_text())
    assert payload["schema"] == "konvid-1k-fetch-manifest-v1"
    assert payload["dataset"] == "konvid-1k"
    assert payload["keep_zips"] is True
    assert len(payload["archives"]) == 2


def test_main_skips_extraction_if_dirs_already_present(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = tmp_path / "konvid"
    root.mkdir()
    # Both zips + both extracted dirs already present.
    (root / "KoNViD_1k_videos.zip").write_bytes(b"\x00" * (FK._VIDEOS_MIN_BYTES + 1))
    (root / "KoNViD_1k_metadata.zip").write_bytes(b"\x00" * (FK._METADATA_MIN_BYTES + 1))
    (root / "KoNViD_1k_videos").mkdir()
    (root / "KoNViD_1k_metadata").mkdir()

    calls: list[tuple] = []
    monkeypatch.setattr(FK, "_extract", lambda *a, **kw: calls.append(a))
    monkeypatch.setattr("sys.argv", ["fetch_konvid_1k.py", "--root", str(root), "--keep-zips"])

    rc = FK.main()
    assert rc == 0
    assert calls == []  # both already-extracted dirs → no _extract call
