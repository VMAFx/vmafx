# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Integration tests for ADR-0543 — ADR-0498 enforcement hardening.

ADR-0498 introduced the explicit-backend gate (``--backend NAME`` init
failure must hard-fail instead of silently falling back to CPU).
ADR-0543 hardens that gate with three contracts the tests below pin:

* **Exit code 100** (``VMAF_EXIT_BACKEND_INIT_FAILED``) for explicit-
  backend init failures — distinct from the generic non-zero return
  so CI gates can ``[[ $rc -eq 100 ]]`` without parsing stderr.
* **Structured JSON error descriptor** at the ``--output`` path when
  format is JSON: ``{"error", "backend_requested", "errno", "adr",
  "exit_code"}``. Empty / pre-existing content is overwritten.
* **Per-feature backend gate**: ``--feature *_cuda`` /
  ``*_sycl`` / ``*_vulkan`` / ``*_hip`` / ``*_metal`` must hard-fail
  when the matching backend isn't active in this run.

Reuses the binary-resolution + YUV-fixture helpers from the V5-1
test in ``test_bbb_e2e_v5_bug_cluster.py`` for parity with the
existing ADR-0498 regression test (V5-1 also probes
``VMAF_BIN_FOR_TESTS`` and the canonical ``build/tools/vmaf``
location). Skips cleanly when no binary is reachable.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))


def _resolve_vmaf_binary() -> Path | None:
    """Locate a built libvmaf CLI binary for the integration test.

    Lookup order mirrors V5-1 (``test_bbb_e2e_v5_bug_cluster.py``):
    ``$VMAF_BIN_FOR_TESTS`` env override, ``shutil.which("vmaf")``,
    then walk-up to ``build/tools/vmaf`` under the nearest repo root.
    """
    env = os.environ.get("VMAF_BIN_FOR_TESTS")
    if env:
        env_path = Path(env)
        if env_path.is_file() and os.access(env_path, os.X_OK):
            return env_path
    which = shutil.which("vmaf")
    if which:
        return Path(which)
    for parent in [_HERE, *_HERE.parents]:
        if (parent / "meson.build").is_file() and (parent / "libvmaf").is_dir():
            candidate = parent / "build" / "tools" / "vmaf"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
            break
    return None


def _find_yuv_resource_root() -> Path | None:
    for parent in [_HERE, *_HERE.parents]:
        target = parent / "python" / "test" / "resource" / "yuv"
        if target.is_dir():
            return target
    return None


def _yuv_pair() -> tuple[Path, Path] | None:
    yuv_root = _find_yuv_resource_root()
    if yuv_root is None:
        return None
    ref = yuv_root / "src01_hrc00_576x324.yuv"
    dist = yuv_root / "src01_hrc01_576x324.yuv"
    if not ref.exists() or not dist.exists():
        return None
    return ref, dist


# ---------------------------------------------------------------------------
# Exit code 100 contract
# ---------------------------------------------------------------------------


def _run_backend(backend: str, output_path: Path | None = None) -> subprocess.CompletedProcess:
    binary = _resolve_vmaf_binary()
    if binary is None:
        pytest.skip(
            "no vmaf binary reachable for ADR-0543 (set VMAF_BIN_FOR_TESTS, "
            "ensure 'vmaf' is on PATH, or build with `ninja -C build vmaf`)"
        )
    pair = _yuv_pair()
    if pair is None:
        pytest.skip("Netflix golden YUV fixtures not available")
    ref, dist = pair
    cmd = [
        str(binary),
        "--backend",
        backend,
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        "576",
        "--height",
        "324",
        "-p",
        "420",
        "-b",
        "8",
    ]
    if output_path is not None:
        cmd += ["--json", "--output", str(output_path)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


@pytest.mark.parametrize("backend", ["sycl", "hip", "cuda", "metal"])
def test_adr_0543_explicit_backend_failure_exits_100(backend: str, tmp_path: Path) -> None:
    """Explicit ``--backend NAME`` init failure must exit 100 (ADR-0543).

    Pre-ADR-0543 the same path printed the refusal message but exited
    255 (= ``int -1`` truncated to ``uint8_t``). Consumers that wanted
    to distinguish backend failures from other non-zero exits had to
    parse stderr for the marker string. ADR-0543 promotes the exit
    code to a stable public contract: ``VMAF_EXIT_BACKEND_INIT_FAILED
    = 100``.

    The test skips when the host has a working device for the
    requested backend (rc=0 + scores returned) because the refusal
    path doesn't fire — that's expected behaviour, not a regression.
    """
    output = tmp_path / f"adr0541_{backend}.json"
    proc = _run_backend(backend, output)
    refused = "refusing to silently fall back" in proc.stderr
    not_compiled = "was built without" in proc.stderr
    if proc.returncode == 0 and not refused and not not_compiled:
        pytest.skip(f"host has a working {backend} device; refusal path not exercised")
    assert proc.returncode == 100, (
        f"ADR-0543 regression: --backend {backend} refusal exited "
        f"{proc.returncode} (expected 100); stderr={proc.stderr!r}"
    )


# ---------------------------------------------------------------------------
# Structured JSON error descriptor contract
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("backend", ["sycl", "hip", "cuda", "metal"])
def test_adr_0543_failure_writes_structured_json(backend: str, tmp_path: Path) -> None:
    """``--output X.json`` must contain a structured error on backend failure.

    Pre-ADR-0543 the file was either never written (path didn't exist)
    or left as a 0-byte file (consumer pre-touched the path). Either
    case forced downstream wrappers to fall back to stderr parsing.
    ADR-0543 overwrites the path with a single-line JSON object
    carrying ``"error"`` / ``"backend_requested"`` / ``"errno"`` /
    ``"adr"`` / ``"exit_code"`` so the consumer can decode the
    failure structurally.
    """
    output = tmp_path / f"adr0541_{backend}.json"
    # Pre-touch the file to mirror the wrapper pattern that V2-E hit.
    output.write_text("{}")
    proc = _run_backend(backend, output)
    refused = "refusing to silently fall back" in proc.stderr
    not_compiled = "was built without" in proc.stderr
    if proc.returncode == 0 and not refused and not not_compiled:
        pytest.skip(f"host has a working {backend} device; refusal path not exercised")
    assert output.exists(), f"JSON output {output} not written"
    payload = json.loads(output.read_text())
    assert "error" in payload, f"missing 'error' key: {payload!r}"
    assert payload.get("backend_requested") == backend, payload
    assert payload.get("adr") == "ADR-0498", payload
    assert payload.get("exit_code") == 100, payload
    # errno is the underlying state_init return, negative when
    # state_init was called, 0 for the not-compiled-in branch.
    assert isinstance(payload.get("errno"), int), payload


# ---------------------------------------------------------------------------
# Per-feature backend gate contract
# ---------------------------------------------------------------------------


def test_adr_0543_per_feature_pinned_to_inactive_backend_fails(tmp_path: Path) -> None:
    """``--feature motion_hip`` must hard-fail when HIP isn't active.

    Pre-ADR-0543, ``vmaf_use_feature("motion_hip")`` silently registered
    the CPU twin when HIP wasn't compiled in / requested. The user got
    a clean exit and a JSON that *looked* like HIP scores but was
    actually computed on the CPU — the exact silent-fallback bug
    ADR-0498 banned for ``--backend NAME``. ADR-0543 closes the gap
    by hard-failing any GPU-pinned feature name when the matching
    backend isn't active.
    """
    binary = _resolve_vmaf_binary()
    if binary is None:
        pytest.skip(
            "no vmaf binary reachable for ADR-0543 (set VMAF_BIN_FOR_TESTS, "
            "ensure 'vmaf' is on PATH, or build with `ninja -C build vmaf`)"
        )
    pair = _yuv_pair()
    if pair is None:
        pytest.skip("Netflix golden YUV fixtures not available")
    ref, dist = pair
    output = tmp_path / "adr0541_motion_hip.json"
    cmd = [
        str(binary),
        "--backend",
        "cpu",  # forces all GPU backends off
        "--feature",
        "motion_hip",
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        "576",
        "--height",
        "324",
        "-p",
        "420",
        "-b",
        "8",
        "--json",
        "--output",
        str(output),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    assert proc.returncode == 100, (
        f"ADR-0543 regression: --feature motion_hip on --backend cpu exited "
        f"{proc.returncode} (expected 100); stderr={proc.stderr!r}"
    )
    assert "pinned to hip backend" in proc.stderr, proc.stderr
    payload = json.loads(output.read_text())
    assert payload.get("backend_requested") == "hip", payload
    assert payload.get("exit_code") == 100, payload


# ---------------------------------------------------------------------------
# Source-level guards (no binary required) — catch refactor regressions
# ---------------------------------------------------------------------------


def _vmaf_c_source() -> str:
    for parent in [_HERE, *_HERE.parents]:
        candidate = parent / "libvmaf" / "tools" / "vmaf.c"
        if candidate.is_file():
            return candidate.read_text()
    pytest.skip("core/tools/vmaf.c not found in any ancestor")
    return ""  # unreachable; satisfies type checker


def test_adr_0543_exit_code_constant_defined() -> None:
    """The dedicated exit code must remain defined at exactly 100."""
    src = _vmaf_c_source()
    assert "#define VMAF_EXIT_BACKEND_INIT_FAILED 100" in src, (
        "ADR-0543 contract: VMAF_EXIT_BACKEND_INIT_FAILED must be defined as 100. "
        "If you renamed or repurposed this macro, also update ADR-0543 and the "
        "Python integration tests in this file."
    )


def test_adr_0543_error_json_helper_wired_into_every_backend() -> None:
    """Every explicit-backend stanza must call ``write_backend_error_json``.

    A refactor that drops the helper from one backend would silently
    revert that backend to the empty-file ADR-0498 behaviour. The
    test grep-asserts the helper is invoked for every supported
    backend keyword.
    """
    src = _vmaf_c_source()
    # Vulkan was dropped from this list by ADR-0726 (2026-05-28).
    for backend in ("sycl", "cuda", "hip", "metal"):
        marker = f'write_backend_error_json(c->output_path, c->output_fmt, "{backend}"'
        assert marker in src, (
            f"ADR-0543 contract: write_backend_error_json must be called for "
            f"--backend {backend} init failures (missing marker: {marker!r})"
        )
    # The per-feature gate emits the helper with a dynamic backend
    # keyword from the feature suffix table, not a literal string.
    assert (
        "feature pinned to inactive backend" in src
    ), "ADR-0543 contract: per-feature backend gate message missing"
