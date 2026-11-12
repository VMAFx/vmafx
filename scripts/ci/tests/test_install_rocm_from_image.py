# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/install-rocm-from-image.sh layer streaming and retry."""

from __future__ import annotations

import gzip
import io
import json
import os
import stat
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SCRIPT = REPO_ROOT / "scripts" / "ci" / "install-rocm-from-image.sh"
SCRIPT_PATH = Path(os.environ.get("VMAFX_INSTALL_ROCM_SCRIPT", str(DEFAULT_SCRIPT)))
SCRIPT_TIMEOUT_S = 60

ROCM_DIGEST = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
EMPTY_DIGEST = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

EXPECTED_RETRY_CALL_COUNT = 2
NEGATIVE_ATTEMPT_COUNT = 3
EXPECTED_BOUNDARY_CALL_COUNT = 1
CURL_PARTIAL_EXIT = 92

CURL_STUB = """#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

state_dir = Path(os.environ["FAKE_CURL_STATE_DIR"])
args = sys.argv[1:]
output = None
for index, arg in enumerate(args):
    if arg in ("-o", "--output") and index + 1 < len(args):
        output = args[index + 1]
    elif arg.startswith("--output="):
        output = arg.partition("=")[2]

def write_body(data):
    if output is None or output == "-":
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    else:
        Path(output).write_bytes(data)

url = None
for arg in args:
    if arg.startswith("http://") or arg.startswith("https://"):
        url = arg
        break

if not url:
    for arg in args:
        if "token" in arg or "/manifests/" in arg or "/blobs/" in arg:
            url = arg
            break

if not url:
    sys.exit(1)

if "token" in url or "auth" in url:
    write_body(json.dumps({"token": "t"}).encode())
    sys.exit(0)

if "/manifests/" in url:
    manifest_file = state_dir / "manifest.json"
    write_body(manifest_file.read_bytes())
    sys.exit(0)

if "/blobs/" in url:
    digest = url.split("/blobs/")[-1]
    safe_digest = digest.replace(":", "_")
    counter_file = state_dir / f"count_{safe_digest}.txt"
    count = 0
    if counter_file.exists():
        count = int(counter_file.read_text(encoding="utf-8").strip())
    count += 1
    counter_file.write_text(str(count), encoding="utf-8")

    mode_file = state_dir / f"mode_{safe_digest}.txt"
    if not mode_file.exists():
        mode_file = state_dir / "mode.txt"
    mode = mode_file.read_text(encoding="utf-8").strip() if mode_file.exists() else "succeed"

    blob_file = state_dir / f"blob_{safe_digest}.tar.gz"
    if not blob_file.exists():
        blob_file = state_dir / "rocm.tar.gz"
    data = blob_file.read_bytes()

    if mode == "fail_once":
        if count == 1:
            write_body(data[: len(data) // 2])
            sys.exit(92)
        write_body(data)
        sys.exit(0)
    elif mode == "always_fail":
        write_body(data[: len(data) // 2])
        sys.exit(92)
    elif mode == "succeed":
        write_body(data)
        sys.exit(0)
    else:
        sys.exit(1)

sys.exit(0)
"""


def _make_rocm_layer() -> bytes:
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w") as tar:
        hipcc_content = b"#!/bin/sh\nexit 0\n"
        ti_hipcc = tarfile.TarInfo("opt/rocm/core-10.1/bin/hipcc")
        ti_hipcc.size = len(hipcc_content)
        ti_hipcc.mode = 0o755
        ti_hipcc.type = tarfile.REGTYPE
        tar.addfile(ti_hipcc, io.BytesIO(hipcc_content))

        hipconfig_content = b"#!/bin/sh\necho 10.1.0\n"
        ti_hipconfig = tarfile.TarInfo("opt/rocm/core-10.1/bin/hipconfig")
        ti_hipconfig.size = len(hipconfig_content)
        ti_hipconfig.mode = 0o755
        ti_hipconfig.type = tarfile.REGTYPE
        tar.addfile(ti_hipconfig, io.BytesIO(hipconfig_content))

        lib_content = b"/* libamdhip64.so */\n"
        ti_lib = tarfile.TarInfo("opt/rocm/core-10.1/lib/libamdhip64.so")
        ti_lib.size = len(lib_content)
        ti_lib.mode = 0o644
        ti_lib.type = tarfile.REGTYPE
        tar.addfile(ti_lib, io.BytesIO(lib_content))

        ti_bin_link = tarfile.TarInfo("opt/rocm/bin")
        ti_bin_link.type = tarfile.SYMTYPE
        ti_bin_link.linkname = "/etc/alternatives/rocm-bin"
        tar.addfile(ti_bin_link)

        ti_lib_link = tarfile.TarInfo("opt/rocm/lib")
        ti_lib_link.type = tarfile.SYMTYPE
        ti_lib_link.linkname = "/etc/alternatives/rocm-lib"
        tar.addfile(ti_lib_link)

    gz_buf = io.BytesIO()
    with gzip.GzipFile(fileobj=gz_buf, mode="wb") as gz:
        gz.write(tar_buf.getvalue())
    return gz_buf.getvalue()


def _make_non_rocm_layer() -> bytes:
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w") as tar:
        dummy = b"not in opt/rocm\n"
        ti = tarfile.TarInfo("etc/dummy.txt")
        ti.size = len(dummy)
        ti.mode = 0o644
        ti.type = tarfile.REGTYPE
        tar.addfile(ti, io.BytesIO(dummy))

    gz_buf = io.BytesIO()
    with gzip.GzipFile(fileobj=gz_buf, mode="wb") as gz:
        gz.write(tar_buf.getvalue())
    return gz_buf.getvalue()


def _setup_test_env(tmp_path: Path) -> tuple[Path, Path, Path, dict[str, str]]:
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    curl_file = bin_dir / "curl"
    curl_file.write_text(CURL_STUB, encoding="utf-8")
    curl_file.chmod(curl_file.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    state_dir = tmp_path / "curl_state"
    state_dir.mkdir(parents=True, exist_ok=True)
    (state_dir / "rocm.tar.gz").write_bytes(_make_rocm_layer())

    dest_dir = tmp_path / "dest"

    env = dict(os.environ)
    env["PATH"] = f"{bin_dir}:{env['PATH']}"
    env["FAKE_CURL_STATE_DIR"] = str(state_dir)
    env["ROCM_LAYER_RETRY_DELAY"] = "0"
    return bin_dir, state_dir, dest_dir, env


def _write_manifest(state_dir: Path, layers: list[dict[str, Any]]) -> None:
    manifest = {
        "schemaVersion": 2,
        "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
        "layers": layers,
    }
    (state_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


def _get_call_count(state_dir: Path, digest: str) -> int:
    safe_digest = digest.replace(":", "_")
    counter_file = state_dir / f"count_{safe_digest}.txt"
    if counter_file.exists():
        return int(counter_file.read_text(encoding="utf-8").strip())
    return 0


def _run_installer(
    image: str,
    dest_dir: Path,
    env: dict[str, str],
) -> subprocess.CompletedProcess[str]:
    """Execute the ROCm image installer script with the given arguments and environment."""
    cmd = [
        "bash",
        str(SCRIPT_PATH),
        "--image",
        image,
        "--dest",
        str(dest_dir),
    ]
    return subprocess.run(  # noqa: S603 - fixed script with test-owned arguments
        cmd,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout=SCRIPT_TIMEOUT_S,
    )


def test_curl_stub_output_modes(tmp_path: Path) -> None:
    """Send bodies to each curl output spelling, or stdout when none is set."""
    bin_dir, state_dir, _, env = _setup_test_env(tmp_path)
    manifest = b'{"layers": []}'
    (state_dir / "manifest.json").write_bytes(manifest)
    url = "https://registry.example.invalid/v2/rocm/manifests/test"
    curl = str(bin_dir / "curl")
    for index, option in enumerate(("-o", "--output", "--output=")):
        output = tmp_path / f"manifest-{index}.json"
        arguments = [f"{option}{output}"] if option.endswith("=") else [option, str(output)]
        proc = run_command(
            [curl, *arguments, url],
            allowed_executables=(curl,),
            env=env,
            capture_output=True,
            timeout_seconds=60,
        )
        assert proc.returncode == 0
        assert proc.stdout == b""
        assert output.read_bytes() == manifest

    proc = run_command(
        [curl, url],
        allowed_executables=(curl,),
        env=env,
        capture_output=True,
        timeout_seconds=60,
    )
    assert proc.returncode == 0
    assert proc.stdout == manifest

    (state_dir / "mode.txt").write_text("always_fail", encoding="utf-8")
    blob = (state_dir / "rocm.tar.gz").read_bytes()
    output = tmp_path / "partial.tar.gz"
    proc = run_command(
        [curl, "--output", str(output), f"https://registry.example.invalid/blobs/{ROCM_DIGEST}"],
        allowed_executables=(curl,),
        env=env,
        capture_output=True,
        timeout_seconds=60,
    )
    assert proc.returncode == CURL_PARTIAL_EXIT
    assert proc.stdout == b""
    assert output.read_bytes() == blob[: len(blob) // 2]


def test_positive_midstream_retry(tmp_path: Path) -> None:
    """Positive case: mid-stream failure once then success -> exit 0, dest contains the file."""
    _, state_dir, dest_dir, env = _setup_test_env(tmp_path)
    _write_manifest(
        state_dir,
        [
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "digest": ROCM_DIGEST,
                "size": 1234,
            }
        ],
    )
    (state_dir / "mode.txt").write_text("fail_once", encoding="utf-8")

    proc = _run_installer(f"rocm/dev-ubuntu-24.04@{ROCM_DIGEST}", dest_dir, env)
    assert proc.returncode == 0, f"Script failed:\nstdout: {proc.stdout}\nstderr: {proc.stderr}"
    assert (dest_dir / "bin" / "hipcc").is_file()
    assert os.access(dest_dir / "bin" / "hipcc", os.X_OK)
    assert _get_call_count(state_dir, ROCM_DIGEST) == EXPECTED_RETRY_CALL_COUNT


def test_negative_blob_always_truncated(tmp_path: Path) -> None:
    """Negative case: blob always truncated -> exit != 0 and stderr names layer and attempt count."""
    _, state_dir, dest_dir, env = _setup_test_env(tmp_path)
    env["ROCM_LAYER_ATTEMPTS"] = str(NEGATIVE_ATTEMPT_COUNT)
    _write_manifest(
        state_dir,
        [
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "digest": ROCM_DIGEST,
                "size": 1234,
            }
        ],
    )
    (state_dir / "mode.txt").write_text("always_fail", encoding="utf-8")

    proc = _run_installer(f"rocm/dev-ubuntu-24.04@{ROCM_DIGEST}", dest_dir, env)
    assert proc.returncode != 0
    assert ROCM_DIGEST in proc.stderr
    assert "download exit 92" in proc.stderr
    assert f"after {NEGATIVE_ATTEMPT_COUNT} attempts" in proc.stderr
    assert _get_call_count(state_dir, ROCM_DIGEST) == NEGATIVE_ATTEMPT_COUNT


def test_boundary_empty_layer_not_retried(tmp_path: Path) -> None:
    """Boundary case: a layer without opt/rocm (tar 'Not found in archive') is NOT retried."""
    _, state_dir, dest_dir, env = _setup_test_env(tmp_path)
    _write_manifest(
        state_dir,
        [
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "digest": EMPTY_DIGEST,
                "size": 100,
            },
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "digest": ROCM_DIGEST,
                "size": 1234,
            },
        ],
    )
    safe_empty = EMPTY_DIGEST.replace(":", "_")
    (state_dir / f"blob_{safe_empty}.tar.gz").write_bytes(_make_non_rocm_layer())
    (state_dir / f"mode_{safe_empty}.txt").write_text("succeed", encoding="utf-8")

    safe_rocm = ROCM_DIGEST.replace(":", "_")
    (state_dir / f"blob_{safe_rocm}.tar.gz").write_bytes(_make_rocm_layer())
    (state_dir / f"mode_{safe_rocm}.txt").write_text("succeed", encoding="utf-8")

    proc = _run_installer(f"rocm/dev-ubuntu-24.04@{ROCM_DIGEST}", dest_dir, env)
    assert proc.returncode == 0, f"Script failed:\nstdout: {proc.stdout}\nstderr: {proc.stderr}"
    assert (dest_dir / "bin" / "hipcc").is_file()
    assert _get_call_count(state_dir, EMPTY_DIGEST) == EXPECTED_BOUNDARY_CALL_COUNT
    assert _get_call_count(state_dir, ROCM_DIGEST) == EXPECTED_BOUNDARY_CALL_COUNT
