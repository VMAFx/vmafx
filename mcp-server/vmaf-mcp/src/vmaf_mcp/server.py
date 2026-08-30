# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""MCP server for the Lusoris VMAF fork.

Exposes fifteen tools over the Model Context Protocol (stdio transport).
The ten core tools are listed below; the P1 tools ``list_extractors``,
``describe_model``, ``run_compare``, ``run_ladder``, and ``run_tune_per_shot``
(ADR-0608) round out the surface:

- ``vmaf_score``            — score a (reference, distorted) raw YUV pair.
- ``vmaf_score_encoded``    — decode encoded video (MP4/MKV/Y4M/…) via ffmpeg then
  score, wrapping ``vmaf_score`` (ADR-0608).
- ``list_models``           — enumerate the VMAF models registered with the build.
- ``list_backends``         — report which backends (cpu/cuda/sycl/hip/metal)
  are compiled into the local vmaf binary.
- ``probe_backend``         — run a 1-frame health check to distinguish
  "compiled in" from "driver present + functional" (ADR-0608).
- ``vmaf_version``          — return the vmaf binary identity and build flags
  (ADR-0608).
- ``run_benchmark``         — run the Netflix benchmark harness (bench_all.sh) across
  all backends.
- ``eval_model_on_split``   — run an ONNX tiny-AI model against a parquet feature
  cache on a deterministic split and report PLCC/SROCC/RMSE.
- ``compare_models``        — rank several ONNX models on the same split.
- ``describe_worst_frames`` — score a pair, pick the N worst-VMAF frames, and
  describe the visible artefacts via SmolVLM / Moondream2 (ADR-0172 / T6-6;
  requires the ``vlm`` extras for actual descriptions, otherwise returns
  frame metadata only).

The server assumes ``build/tools/vmaf`` exists (build first with
``meson compile -C build``). Paths are validated to live under either the
repository's ``testdata/`` / ``python/test/resource/`` / ``model/`` trees
or an explicitly-allowlisted prefix passed via ``VMAF_MCP_ALLOW``. This
prevents callers from coercing the server into reading arbitrary host
paths.
"""

from __future__ import annotations

# NOTE (risk-accept): the `subprocess` import below exec's our own signed
# vmaf binary with an argv list (no shell=True, no user-controlled
# strings in argv[0]); broad exception handlers on the call paths
# convert failures into JSON-RPC errors for the client. If ruff `select`
# is ever widened to include the bandit (`S`) or blind-except (`BLE`)
# rules, re-evaluate these sites deliberately rather than silencing
# with line-level suppression markers.
import asyncio
import contextlib
import json
import logging
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

_logger = logging.getLogger(__name__)

# Concurrency cap for vmaf subprocess spawning.  Unbounded concurrent requests
# exhaust memory (each vmaf process loads the model + allocates frame buffers).
# The default of 8 is a safe upper bound on a single GPU or a 16-core CPU host;
# operators can raise it via VMAF_MCP_MAX_CONCURRENT when running on larger
# hardware.  Reads the env var at import time so the value is fixed for the
# lifetime of the server process.
_SCORE_SEM: asyncio.Semaphore = asyncio.Semaphore(int(os.environ.get("VMAF_MCP_MAX_CONCURRENT", 8)))


# Default per-tool wall-clock timeout (seconds) for any awaited subprocess
# `communicate()` call. Each async subprocess site below funnels through
# `_communicate_with_timeout` so a hung child cannot wedge an MCP tool
# indefinitely. The default of 600 s (10 min) is generous enough for the
# heaviest in-process workload (bench_all.sh runs ~30–120 s, vmaf-tune
# ladder builds can run several minutes) while still putting a finite cap
# on hangs.  Override at deploy time with `VMAF_MCP_SUBPROCESS_TIMEOUT_S`
# (any positive int or float, decimals OK).
def _subprocess_timeout_s() -> float:
    """Return the configured subprocess timeout in seconds.

    Re-reads the env var on every call so test suites can override it
    per-test without re-importing the module.
    """
    raw = os.environ.get("VMAF_MCP_SUBPROCESS_TIMEOUT_S")
    if not raw:
        return 600.0
    try:
        value = float(raw)
    except ValueError:
        return 600.0
    return value if value > 0.0 else 600.0


async def _communicate_with_timeout(
    proc: asyncio.subprocess.Process,
    timeout: float | None = None,
) -> tuple[bytes, bytes]:
    """Await ``proc.communicate()`` with a wall-clock @p timeout.

    On :class:`asyncio.TimeoutError` the child process is killed (after a
    best-effort drain) and a :class:`RuntimeError` is raised.  On
    :class:`asyncio.CancelledError` (client disconnected mid-stream) the
    child process is also killed so it does not linger as an orphan consuming
    CPU or GPU resources after the MCP tool coroutine is cancelled (ADR-1085).
    The default timeout comes from :func:`_subprocess_timeout_s` (see
    ``VMAF_MCP_SUBPROCESS_TIMEOUT_S``).  Mirrors the existing per-call
    timeouts on the synchronous ``subprocess.run`` sites so async and sync
    paths have symmetric hang protection.
    """
    deadline = timeout if timeout is not None else _subprocess_timeout_s()
    try:
        return await asyncio.wait_for(proc.communicate(), timeout=deadline)
    except asyncio.TimeoutError as exc:
        with contextlib.suppress(ProcessLookupError):
            proc.kill()
        # Best-effort drain so the OS releases the pipes; ignore further
        # timeouts here — the process is already going away.
        with contextlib.suppress(asyncio.TimeoutError, ProcessLookupError):
            await asyncio.wait_for(proc.communicate(), timeout=5.0)
        raise RuntimeError(
            f"subprocess timed out after {deadline:.1f}s (set "
            "VMAF_MCP_SUBPROCESS_TIMEOUT_S to override)"
        ) from exc
    except asyncio.CancelledError:
        # The MCP tool coroutine was cancelled (e.g. client disconnected).
        # Kill the child process so it does not run to completion as an orphan.
        with contextlib.suppress(ProcessLookupError):
            proc.kill()
        # Best-effort drain so the OS releases the pipes.
        with contextlib.suppress(asyncio.TimeoutError, ProcessLookupError):
            await asyncio.wait_for(proc.communicate(), timeout=5.0)
        # Re-raise so asyncio can properly propagate the cancellation.
        raise


# ---------------------------------------------------------------------------
# Strict JSON serialisation (ADR-0988)
# ---------------------------------------------------------------------------


def _nan_to_none(value: Any, *, _max_depth: int = 200) -> Any:
    """Replace non-finite floats with ``None`` using an iterative stack walk.

    Python's default ``json.dumps`` (``allow_nan=True``) emits bare ``NaN`` /
    ``Infinity`` tokens that are not valid RFC 8259 JSON.  MCP clients that use
    strict parsers (Go ``encoding/json``, Rust ``serde_json``, ``jq``) will
    reject the response.  Coercing to ``null`` is the portable fix.

    The canonical implementation lives in ``vmaftune.jsonio``; this copy is
    intentionally inlined here because ``vmaf-mcp`` does not declare
    ``vmaf-tune`` as a dependency (ADR-0988).

    This implementation is fully iterative (explicit work-stack, no Python
    call-stack recursion) so it never triggers ``RecursionError`` regardless of
    input nesting depth.  Subtrees rooted deeper than ``_max_depth`` (default
    200) are replaced with ``None`` to bound both memory and traversal time on
    adversarial inputs.

    Algorithm: the work-stack carries ``(node, depth, parent_out, key)`` tuples.
    Each pop either (a) writes a scalar directly into the parent container via
    ``parent_out[key] = coerced``, or (b) allocates a fresh output container,
    writes it into the parent, then pushes all children onto the stack so they
    fill the new container on subsequent iterations.  The root result is
    collected via a sentinel single-element list used as the top-level "parent".
    """
    # Sentinel single-element list used as the root parent container.
    # After the loop, root_out[0] holds the transformed tree.
    root_out: list[Any] = [None]

    # Stack entries: (node, depth, parent_out, key)
    # parent_out[key] is where this node's result must be written.
    stack: list[tuple[Any, int, Any, Any]] = [(value, 0, root_out, 0)]

    while stack:
        node, depth, parent_out, key = stack.pop()

        # Depth-exceeded or non-container scalars: write result directly.
        if depth > _max_depth:
            parent_out[key] = None
            continue

        if isinstance(node, float):
            parent_out[key] = None if (math.isnan(node) or math.isinf(node)) else node
            continue

        if not isinstance(node, (dict, list, tuple)):
            # int, str, bool, NoneType, etc. — pass through unchanged.
            parent_out[key] = node
            continue

        # Container node: allocate output container, link it to parent, then
        # push all children so they are filled on subsequent iterations.
        if isinstance(node, dict):
            out: Any = {}
            parent_out[key] = out
            for k, child in node.items():
                stack.append((child, depth + 1, out, k))
        else:
            # list or tuple — always rebuild as list (json.dumps treats both alike).
            out = [None] * len(node)
            parent_out[key] = out
            for i, child in enumerate(node):
                stack.append((child, depth + 1, out, i))

    return root_out[0]


def _dumps_strict(data: Any, *, indent: int | None = 2) -> str:
    """Emit RFC 8259-compliant JSON with non-finite floats rendered as null."""
    return json.dumps(_nan_to_none(data), indent=indent, sort_keys=True, allow_nan=False)


# ---------------------------------------------------------------------------
# Configuration & path validation
# ---------------------------------------------------------------------------


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def _vmaf_binary() -> Path:
    """Return the path to the vmaf binary.

    Resolution order:
    1. VMAF_BIN environment variable (explicit override).
    2. /usr/local/bin/vmaf -- installed by make install or the container.
    3. <repo>/core/build/tools/vmaf -- in-tree fork build.
    4. <repo>/build/tools/vmaf -- legacy build-dir name.

    Returns the first candidate that exists on disk. If none exist the
    last candidate path is returned so the caller can emit a clear error.
    """
    env = os.environ.get("VMAF_BIN")
    if env:
        return Path(env)

    candidates = [
        Path("/usr/local/bin/vmaf"),
        _repo_root() / "core" / "build" / "tools" / "vmaf",
        _repo_root() / "build" / "tools" / "vmaf",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    # None found -- return the most-likely path for a clear error message.
    return candidates[1]


def _allowed_roots() -> list[Path]:
    """Return the list of filesystem trees under which MCP tool paths are allowed.

    Default roots:
    - ``<repo>/testdata`` — fork-added YUV fixtures and benchmark harnesses.
    - ``<repo>/python/test/resource`` — Netflix golden fixture tree (includes
      ``yuv/``, ``model/``, and ``feature/`` sub-directories).
    - ``<repo>/model`` — shipped VMAF model JSON/PKL/ONNX files.
    - ``/workspace/python/test/resource/yuv`` — vmaf-dev-mcp container mount
      (ADR-0496 / Bug B, 2026-05-18): the container bind-mounts the repo
      at ``/workspace/``, so the absolute container path must also be
      allowlisted so MCP calls that pass the absolute container path for the
      Netflix golden YUVs succeed without requiring ``VMAF_MCP_ALLOW``.

    Additional roots may be added at runtime via the ``VMAF_MCP_ALLOW``
    environment variable (colon-separated list of absolute paths).
    """
    roots = [
        _repo_root() / "testdata",
        _repo_root() / "python" / "test" / "resource",
        _repo_root() / "model",
        # Bug B fix: absolute container path for the Netflix golden YUVs
        # (vmaf-dev-mcp container, ADR-0496). The bind-mount makes the
        # repo root appear at /workspace/, so the golden YUVs live at
        # /workspace/python/test/resource/yuv/ inside the container.
        Path("/workspace/python/test/resource"),
    ]
    extra = os.environ.get("VMAF_MCP_ALLOW")
    if extra:
        roots.extend(Path(p).resolve() for p in extra.split(":") if p)
    return [r.resolve() for r in roots]


def _validate_path(p: str) -> Path:
    path = Path(p).resolve()
    allowed = _allowed_roots()
    if not any(path.is_relative_to(root) for root in allowed):
        raise ValueError(
            f"path {path} not under an allowlisted root; set VMAF_MCP_ALLOW to extend."
        )
    if not path.is_file():
        raise FileNotFoundError(str(path))
    return path


# Schemes permitted for media source paths passed to vmaf-tune subcommands.
# Only local file paths are allowed; remote schemes (http, https, rtsp, s3,
# rclone://, etc.) are rejected to prevent SSRF via ffmpeg's URL demuxer.
_ALLOWED_MEDIA_SCHEMES: frozenset[str] = frozenset({"file"})


def _validate_media_path(p: str) -> str:
    """Validate a media source path for vmaf-tune subcommands.

    Unlike :func:`_validate_path`, this function does NOT require the path
    to already exist on disk (vmaf-tune may open a container file that the
    MCP server cannot stat independently), but it applies the same allowlist
    and symlink-resolution rules.

    Rejects:
    - Paths containing null bytes (argv injection vector).
    - URL schemes other than ``file://`` (blocks SSRF via ffmpeg URL demuxer).
    - Paths not under any allowlisted root after ``Path.resolve()`` (blocks
      directory-traversal escape via ``../`` sequences and symlink chains).

    Returns the validated path string suitable for passing to subprocess argv.
    Raises ``ValueError`` on any violation.
    """
    if "\x00" in p:
        raise ValueError("media path must not contain null bytes")

    # Reject non-file URL schemes (http://, rtsp://, s3://, rclone://, etc.)
    # that ffmpeg would silently follow as remote sources.
    colon_pos = p.find("://")
    if colon_pos != -1:
        scheme = p[:colon_pos].lower()
        if scheme not in _ALLOWED_MEDIA_SCHEMES:
            raise ValueError(
                f"media path scheme '{scheme}://' is not permitted; "
                "only local file paths are accepted."
            )
        # Strip the file:// prefix so the rest of the checks apply to the path.
        p = p[len("file://") :]

    resolved = Path(p).resolve()
    allowed = _allowed_roots()
    if not any(resolved.is_relative_to(root) for root in allowed):
        raise ValueError(
            f"media path {resolved} not under an allowlisted root; " "set VMAF_MCP_ALLOW to extend."
        )
    return str(resolved)


# ---------------------------------------------------------------------------
# Tool implementations
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ScoreExtras:
    """Optional scoring pass-through flags shared by vmaf_score +
    vmaf_score_encoded (ADR-1117).

    Each field maps onto a ``vmaf`` CLI flag verified against
    ``core/tools/cli_parse.c``. A default-constructed ``ScoreExtras`` adds no
    flags (backward-compatible). MUST stay byte-compatible with the Go
    server's ``scoreExtras`` argv construction (cmd/vmafx-mcp/impl.go).
    """

    features: tuple[str, ...] = ()  # repeated --feature
    aom_ctc: str | None = None  # --aom_ctc
    nflx_ctc: str | None = None  # --nflx_ctc
    tiny_model: str | None = None  # --tiny-model
    tiny_device: str | None = None  # --tiny-device (alias --dnn-ep)
    tiny_threads: int | None = None  # --tiny-threads
    tiny_fp16: bool = False  # --tiny-fp16
    tiny_model_verify: bool = False  # --tiny-model-verify
    tiny_codec: str | None = None  # --tiny-codec
    tiny_preset: str | None = None  # --tiny-preset
    tiny_crf: int | None = None  # --tiny-crf
    tiny_resize: str | None = None  # --tiny-resize
    no_reference: bool = False  # --no-reference
    threads: int | None = None  # --threads
    frame_cnt: int | None = None  # --frame_cnt
    frame_skip_ref: int | None = None  # --frame_skip_ref
    frame_skip_dist: int | None = None  # --frame_skip_dist
    no_prediction: bool = False  # --no_prediction

    def is_empty(self) -> bool:
        """Return True when no extra flag is set."""
        return self == ScoreExtras()

    def to_argv(self) -> list[str]:
        """Build the flag list, in a fixed order matching the Go server."""
        argv: list[str] = []
        for feat in self.features:
            argv += ["--feature", feat]
        if self.aom_ctc:
            argv += ["--aom_ctc", self.aom_ctc]
        if self.nflx_ctc:
            argv += ["--nflx_ctc", self.nflx_ctc]
        if self.tiny_model:
            argv += ["--tiny-model", self.tiny_model]
        if self.tiny_device:
            argv += ["--tiny-device", self.tiny_device]
        if self.tiny_threads is not None:
            argv += ["--tiny-threads", str(self.tiny_threads)]
        if self.tiny_fp16:
            argv += ["--tiny-fp16"]
        if self.tiny_model_verify:
            argv += ["--tiny-model-verify"]
        if self.tiny_codec:
            argv += ["--tiny-codec", self.tiny_codec]
        if self.tiny_preset:
            argv += ["--tiny-preset", self.tiny_preset]
        if self.tiny_crf is not None:
            argv += ["--tiny-crf", str(self.tiny_crf)]
        if self.tiny_resize:
            argv += ["--tiny-resize", self.tiny_resize]
        if self.no_reference:
            argv += ["--no-reference"]
        if self.threads is not None:
            argv += ["--threads", str(self.threads)]
        if self.frame_cnt is not None:
            argv += ["--frame_cnt", str(self.frame_cnt)]
        if self.frame_skip_ref is not None:
            argv += ["--frame_skip_ref", str(self.frame_skip_ref)]
        if self.frame_skip_dist is not None:
            argv += ["--frame_skip_dist", str(self.frame_skip_dist)]
        if self.no_prediction:
            argv += ["--no_prediction"]
        return argv


@dataclass(frozen=True)
class ScoreRequest:
    ref: Path | None
    dis: Path
    width: int
    height: int
    pixfmt: str  # "420" | "422" | "444"
    bitdepth: int
    model: str = "version=vmaf_v0.6.1"
    backend: str = "auto"  # "cpu" | "cuda" | "sycl" | "auto"
    precision: str = "legacy"  # "legacy" = %.6f; matches C CLI default per ADR-0119
    subsample: int = 1  # score every Nth frame; passed as --subsample to the CLI
    extras: ScoreExtras = ScoreExtras()  # optional pass-through flags (ADR-1117)


def _extras_from_args(arguments: dict[str, Any]) -> ScoreExtras:
    """Build a :class:`ScoreExtras` from a raw tool-call ``arguments`` dict.

    Only keys that are present are forwarded; absent keys leave the field at
    its default (``None`` / ``False`` / ``()``), so no CLI flag is emitted.
    Mirrors the Go server's ``parseScoreExtras`` (cmd/vmafx-mcp/impl.go).
    """
    raw_features = arguments.get("feature")
    features: tuple[str, ...] = ()
    if isinstance(raw_features, list):
        features = tuple(str(f) for f in raw_features if isinstance(f, str) and f)

    def _opt_int(key: str) -> int | None:
        return int(arguments[key]) if key in arguments else None

    def _opt_str(key: str) -> str | None:
        return str(arguments[key]) if key in arguments else None

    return ScoreExtras(
        features=features,
        aom_ctc=_opt_str("aom_ctc"),
        nflx_ctc=_opt_str("nflx_ctc"),
        tiny_model=_opt_str("tiny_model"),
        tiny_device=_opt_str("tiny_device"),
        tiny_threads=_opt_int("tiny_threads"),
        tiny_fp16=bool(arguments.get("tiny_fp16", False)),
        tiny_model_verify=bool(arguments.get("tiny_model_verify", False)),
        tiny_codec=_opt_str("tiny_codec"),
        tiny_preset=_opt_str("tiny_preset"),
        tiny_crf=_opt_int("tiny_crf"),
        tiny_resize=_opt_str("tiny_resize"),
        no_reference=bool(arguments.get("no_reference", False)),
        threads=_opt_int("threads"),
        frame_cnt=_opt_int("frame_cnt"),
        frame_skip_ref=_opt_int("frame_skip_ref"),
        frame_skip_dist=_opt_int("frame_skip_dist"),
        no_prediction=bool(arguments.get("no_prediction", False)),
    )


# Map each named backend to the set of siblings it must disable so the
# vmaf binary does not probe and select a different runtime at startup.
# "auto" leaves all probes active (no --no_* flags added).
_BACKEND_DISABLE: dict[str, tuple[str, ...]] = {
    "cpu": ("cuda", "sycl", "hip", "metal"),
    "cuda": ("sycl", "hip", "metal"),
    "sycl": ("cuda", "hip", "metal"),
    "hip": ("cuda", "sycl", "metal"),
    "metal": ("cuda", "sycl", "hip"),
}


# Models trained at a specific resolution preset (substring match on the
# `version=...` selector or a path stem). Used by the resolution-mismatch
# warning emitted from `_run_vmaf_score`. Bug #5 from the 2026-05-17 MCP
# probe: `vmaf_4k_v0.6.1` saturates at 100 on every SD frame and the MCP
# layer silently returned the bogus pool. Surface a hint instead.
_MODEL_RES_HINTS: dict[str, str] = {
    "vmaf_4k": "4k",
    "vmaf_v0.6.1neg": "hd",  # 1080p training set
    "vmaf_v0.6.1": "hd",
    "vmaf_b_v0.6.3": "hd",
}


def _classify_source_resolution(width: int, height: int) -> str:
    """Return ``'sd'`` / ``'hd'`` / ``'4k'`` for the longer-edge bucket."""
    long_edge = max(int(width), int(height))
    if long_edge >= 3840:
        return "4k"
    if long_edge >= 1280:
        return "hd"
    return "sd"


def _model_resolution_class(model: str) -> str | None:
    """Best-effort classification of @p model's intended resolution.

    Looks at the model name embedded in the ``version=NAME`` / ``path=...``
    string. Returns ``None`` when the model is unknown so callers don't
    nag on bespoke ONNX models.
    """
    blob = model.lower()
    for needle, klass in _MODEL_RES_HINTS.items():
        if needle in blob:
            return klass
    return None


def _resolution_mismatch_warning(model: str, width: int, height: int) -> str | None:
    """Return a one-line warning string when the model's intended
    resolution does not match the source frame size, else ``None``."""
    model_class = _model_resolution_class(model)
    if model_class is None:
        return None
    source_class = _classify_source_resolution(width, height)
    if model_class == source_class:
        return None
    return (
        f"model resolution preset {model_class!r} does not match "
        f"source {width}x{height} ({source_class!r}); "
        "scores may saturate or be biased — pick a model trained at the "
        "matching resolution."
    )


# Cache of the host vmaf binary's advertised backends, indexed by absolute
# binary path. Populated lazily on first `_probe_backends` call.
_BACKEND_PROBE_CACHE: dict[str, frozenset[str]] = {}
# Round-5 race fix (finding #7): guards _probe_backends_async against TOCTOU —
# multiple concurrent coroutines all seeing a cache miss and each dispatching
# a blocking subprocess to the thread pool.  A single asyncio.Lock is sufficient
# because the event loop is single-threaded; the double-checked pattern inside
# _probe_backends_async ensures only one subprocess fires per key.
_BACKEND_PROBE_LOCK: asyncio.Lock | None = None


def _get_backend_probe_lock() -> asyncio.Lock:
    """Return (creating on first call) the module-level asyncio.Lock.

    Deferred creation avoids constructing the Lock before the event loop
    is running (e.g. during import-time module initialisation).
    """
    global _BACKEND_PROBE_LOCK
    if _BACKEND_PROBE_LOCK is None:
        _BACKEND_PROBE_LOCK = asyncio.Lock()
    return _BACKEND_PROBE_LOCK


# Per-key asyncio locks used by _probe_backends_async to prevent TOCTOU: if N
# coroutines all see a cache miss simultaneously and each dispatches a thread,
# the subprocess runs N times and the final cached value is undefined-order.
# The first coroutine that acquires the per-key lock does the work; subsequent
# coroutines wait on the same lock and then hit the cache.
_BACKEND_PROBE_LOCKS: dict[str, asyncio.Lock] = {}
# Guards mutation of _BACKEND_PROBE_LOCKS itself (the dict is not thread-safe
# under concurrent asyncio coroutine creation).
_BACKEND_PROBE_LOCK_DICT_LOCK: asyncio.Lock | None = None


def _get_probe_lock_dict_lock() -> asyncio.Lock:
    """Return (lazily creating) the lock that guards _BACKEND_PROBE_LOCKS.

    Deferred creation avoids creating an asyncio.Lock at module import time,
    which would bind it to whatever event loop happens to be current then —
    causing "attached to a different loop" errors in tests that spin up their
    own loops.
    """
    global _BACKEND_PROBE_LOCK_DICT_LOCK
    if _BACKEND_PROBE_LOCK_DICT_LOCK is None:
        _BACKEND_PROBE_LOCK_DICT_LOCK = asyncio.Lock()
    return _BACKEND_PROBE_LOCK_DICT_LOCK


async def _get_or_create_probe_lock(key: str) -> asyncio.Lock:
    """Return a per-*key* asyncio.Lock, creating it if absent.

    All writes to :data:`_BACKEND_PROBE_LOCKS` are serialised through
    :data:`_BACKEND_PROBE_LOCK_DICT_LOCK` so no two coroutines can create
    duplicate lock objects for the same key.
    """
    async with _get_probe_lock_dict_lock():
        if key not in _BACKEND_PROBE_LOCKS:
            _BACKEND_PROBE_LOCKS[key] = asyncio.Lock()
        return _BACKEND_PROBE_LOCKS[key]


def _probe_backends(vmaf: Path) -> frozenset[str]:
    """Return the set of backends the local @p vmaf binary advertises.

    Probes ``vmaf --help`` (which always lists every ``--no_<backend>``
    flag) and matches the documented backend names. ``cpu`` is always
    included — it has no driver dependency and is never gated.

    Result is cached for the lifetime of the server process so we don't
    fork a subprocess per `vmaf_score` call.

    **Blocking note**: this function is synchronous and calls
    ``subprocess.run``.  Async callers must use the companion
    :func:`_probe_backends_async` wrapper so the first-call subprocess
    does not stall the event loop (ADR-1023).
    """
    key = str(vmaf)
    cached = _BACKEND_PROBE_CACHE.get(key)
    if cached is not None:
        return cached
    advertised: set[str] = {"cpu"}
    try:
        result = subprocess.run(
            [str(vmaf), "--help"], capture_output=True, text=True, timeout=5, check=False
        )
    except (subprocess.TimeoutExpired, OSError):
        # On probe failure we conservatively assume CPU-only — better to
        # over-reject and let the user override via env than to silently
        # fall back as bug #1 used to.
        _logger.warning(
            "vmaf --help probe failed; assuming CPU-only backend support",
            exc_info=True,
        )
        probe = frozenset(advertised)
        _BACKEND_PROBE_CACHE[key] = probe
        return probe
    blob = (result.stdout or "") + (result.stderr or "")
    # The CLI documents disable-flags as `--no_<backend>` (one per line).
    for name in ("cuda", "sycl", "hip", "metal"):
        if re.search(rf"--no_{name}\b", blob):
            advertised.add(name)
    probe = frozenset(advertised)
    _BACKEND_PROBE_CACHE[key] = probe
    return probe


async def _probe_backends_async(vmaf: Path) -> frozenset[str]:
    """Async-safe wrapper around :func:`_probe_backends`.

    Returns the cached result immediately on a cache hit (no thread
    hop needed).  On a cache miss the function serialises concurrent
    waiters through a per-*vmaf-path* asyncio.Lock before delegating
    to a thread pool via ``asyncio.to_thread``, so the blocking
    ``subprocess.run`` inside :func:`_probe_backends` runs at most once
    per binary path and does not stall the event loop (ADR-1023).

    Without the lock a TOCTOU window exists: N coroutines can all
    observe a cache miss simultaneously, each dispatch a thread, and
    produce N redundant subprocess invocations whose ordering is
    undefined.  The per-key lock collapses all concurrent waiters onto
    a single probe: the first coroutine acquires the lock, runs the
    probe, populates the cache, then releases; every subsequent
    coroutine acquires the lock, finds the cache already populated, and
    returns immediately.
    """
    key = str(vmaf)
    # Fast path — avoids acquiring either lock when the cache is already warm.
    if key in _BACKEND_PROBE_CACHE:
        return _BACKEND_PROBE_CACHE[key]
    lock = await _get_or_create_probe_lock(key)
    async with lock:
        # Re-check after acquiring the lock: a sibling coroutine may have
        # completed the probe while we were waiting.
        if key in _BACKEND_PROBE_CACHE:
            return _BACKEND_PROBE_CACHE[key]
        return await asyncio.to_thread(_probe_backends, vmaf)


async def _run_vmaf_score(req: ScoreRequest) -> dict[str, Any]:
    vmaf = _vmaf_binary()
    if not vmaf.exists():
        raise RuntimeError(f"vmaf binary not found at {vmaf}. Build first: meson compile -C build.")

    # Bug #1 from the 2026-05-17 MCP probe: caller-requested backend
    # silently fell through to CPU when the binary lacked the runtime.
    # Refuse explicitly (and let auto pass through unchanged).
    # ADR-1023: use the async wrapper to avoid blocking the event loop.
    if req.backend != "auto":
        advertised = await _probe_backends_async(vmaf)
        if req.backend not in advertised:
            raise RuntimeError(
                f"backend {req.backend!r} requested but the local vmaf binary "
                f"does not advertise it (available: {sorted(advertised)}); "
                "refusing to fall back silently. Pass backend='auto' to let "
                "vmaf pick, or rebuild with the requested backend enabled."
            )

    # Concurrency cap: acquire _SCORE_SEM before spawning the vmaf subprocess.
    # Each vmaf process loads the model and allocates per-frame buffers; unbounded
    # concurrency exhausts memory on every host configuration.  Binary validation
    # and backend checks run before acquisition so we fail fast without holding a
    # slot (VMAF_MCP_MAX_CONCURRENT controls the cap, default 8).
    async with _SCORE_SEM:
        # Round 26 A.2: use NamedTemporaryFile to guarantee a unique path.
        # The task-name approach (asyncio.current_task().get_name()) was vulnerable
        # to collision if tasks were renamed, and the name space is small under
        # high concurrency.  delete=False hands ownership to the finally block so
        # the vmaf subprocess can reopen the path by name; the try/finally below
        # unlinks unconditionally.
        with tempfile.NamedTemporaryFile(
            prefix="vmaf-mcp-",
            suffix=".json",
            delete=False,
        ) as _tmp:
            output = Path(_tmp.name)
        try:
            argv = [str(vmaf)]
            # In no-reference mode the reference path may be omitted (only the
            # distorted picture is scored by the NR tiny model). Emit -r only
            # when a reference is supplied — FR mode, or an NR caller passing
            # one. Mirrors the Go server's conditional -r (ADR-1117).
            if req.ref is not None:
                argv += ["-r", str(req.ref)]
            argv += [
                "-d",
                str(req.dis),
                "--width",
                str(req.width),
                "--height",
                str(req.height),
                "-p",
                req.pixfmt,
                "-b",
                str(req.bitdepth),
                "-m",
                req.model,
                "--precision",
                req.precision,
                "-q",
                "-o",
                str(output),
                "--json",
            ]
            if req.subsample > 1:
                argv += ["--subsample", str(req.subsample)]
            # Optional pass-through scoring flags (ADR-1117). Appended before
            # the backend-disable flags so the argv order matches the Go server.
            argv += req.extras.to_argv()
            if req.backend in _BACKEND_DISABLE:
                for sibling in _BACKEND_DISABLE[req.backend]:
                    argv.append(f"--no_{sibling}")

            proc = await asyncio.create_subprocess_exec(
                *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            _stdout, stderr = await _communicate_with_timeout(proc)
            if proc.returncode != 0:
                raise RuntimeError(
                    f"vmaf exited {proc.returncode}: {stderr.decode(errors='replace')}"
                )
            # Pin UTF-8 explicitly so the parse does not pick up the server
            # process's locale (LC_ALL may differ between MCP-stdio launches
            # and CI runners, and a non-UTF-8 default decoder would crash on
            # legitimate accented filenames in the vmaf JSON payload).
            try:
                payload: dict[str, Any] = json.loads(output.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"vmaf output unreadable (disk-full or OOM kill?): {exc}"
                ) from exc
            # Bug #1 (echo): tell the caller which backend actually ran, so
            # downstream parity tests can assert it instead of trusting the
            # request silently.
            payload["backend_requested"] = req.backend
            payload["backend_used"] = (
                req.backend if req.backend != "auto" else _infer_backend_from_payload(payload)
            )
            # Bug #5: surface a resolution-mismatch warning when the model's
            # training resolution preset disagrees with the source frame size.
            warning = _resolution_mismatch_warning(req.model, req.width, req.height)
            if warning is not None:
                payload["mismatched_model_warning"] = warning
            return payload
        finally:
            output.unlink(missing_ok=True)


def _infer_backend_from_payload(payload: dict[str, Any]) -> str:
    """Best-effort backend identification from the vmaf JSON output.

    libvmaf does not (yet) emit a ``backend`` field in its JSON, but the
    per-backend feature-key counts diverge in well-known ways (see the
    `bench_all.sh` `compare` table: CPU 14-15, CUDA 11-12).
    When the count is ambiguous we return ``'cpu'`` — the conservative
    default the CLI also picks when no GPU is wired up.
    """
    frames = payload.get("frames") or []
    if not frames:
        return "cpu"
    metrics = frames[0].get("metrics") or {}
    nkeys = len(metrics)
    if nkeys <= 12:
        # Could be CUDA or SYCL — without a backend marker we cannot
        # disambiguate further. Return 'gpu' as a hint.
        return "gpu"
    return "cpu"


def _list_models() -> list[dict[str, Any]]:
    models_dir = _repo_root() / "model"
    out: list[dict[str, Any]] = []
    for p in sorted(models_dir.rglob("*")):
        if p.suffix in {".json", ".pkl", ".onnx"} and p.is_file():
            out.append(
                {
                    "name": p.stem,
                    "path": str(p.relative_to(_repo_root())),
                    "format": p.suffix.lstrip("."),
                    "size_bytes": p.stat().st_size,
                }
            )
    return out


async def _list_backends() -> dict[str, bool]:
    """Return which backends the local vmaf binary was compiled with.

    Delegates to :func:`_probe_backends_async`, which reads ``vmaf --help``
    and looks for ``--no_<backend>`` flags (presence = compiled in).
    This correctly identifies live CUDA/SYCL/HIP/Metal support
    even on hosts where the ``--version`` banner does not mention GPU
    backends — the historical ``--version`` grep approach (Bug A,
    2026-05-18) missed CUDA on the ``vmaf-dev-mcp`` container because
    the banner omits backend names.

    Bug A fix: the old implementation searched the ``--version`` output
    for keyword substrings which may be absent despite the backend being
    compiled in (e.g. CUDA enabled but no "CUDA" token in the banner).
    ``--help`` always lists ``--no_<backend>`` for every compiled backend.

    ADR-1023: made async so the ``subprocess.run`` inside
    :func:`_probe_backends` does not stall the event loop.
    """
    vmaf = _vmaf_binary()
    if not vmaf.exists():
        # CPU is always available regardless of binary presence; GPU backends
        # cannot be confirmed without running the binary.
        return {
            "cpu": True,
            "cuda": False,
            "sycl": False,
            "hip": False,
            "metal": False,
        }
    advertised = await _probe_backends_async(vmaf)
    return {
        "cpu": True,
        "cuda": "cuda" in advertised,
        "sycl": "sycl" in advertised,
        "hip": "hip" in advertised,
        "metal": "metal" in advertised,
    }


_FEATURE_COLUMNS = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)
_VALID_SPLITS = ("train", "val", "test", "all")


def _eval_model_on_split(
    model: Path, features: Path, split: str, input_name: str
) -> dict[str, Any]:
    """Run @p model on @p split of @p features and return PLCC/SROCC/RMSE.

    Imports are lazy so the base mcp-server install (no pandas / onnxruntime
    / scipy) isn't forced to pull in ML deps just to score video.
    """
    if split not in _VALID_SPLITS:
        raise ValueError(f"split must be one of {_VALID_SPLITS}; got {split!r}")
    try:
        import numpy as np
        import onnxruntime as ort
        import pandas as pd
        from scipy.stats import pearsonr, spearmanr
    except ImportError as exc:  # pragma: no cover — exercised only without extras
        raise RuntimeError(
            "eval_model_on_split requires the 'eval' extra: pip install 'vmaf-mcp[eval]'"
        ) from exc

    df = pd.read_parquet(features)
    if "mos" not in df.columns:
        raise ValueError(f"{features} has no 'mos' column — can't score correlations")
    if split != "all" and "key" in df.columns:
        # Inline the split_keys hashing so we don't depend on vmaf_train.
        import hashlib

        def bucket(key: str) -> float:
            h = hashlib.sha256(f"vmaf-train-splits-v1:{key}".encode()).digest()
            return int.from_bytes(h[:8], "big") / (1 << 64)

        val_frac, test_frac = 0.1, 0.1

        def which(key: str) -> str:
            b = bucket(str(key))
            if b < test_frac:
                return "test"
            if b < test_frac + val_frac:
                return "val"
            return "train"

        keep = df["key"].astype(str).map(which) == split
        df = df[keep]

    cols = [c for c in _FEATURE_COLUMNS if c in df.columns]
    if not cols:
        raise ValueError(
            f"{features} has none of the expected feature columns "
            f"{_FEATURE_COLUMNS}; got {list(df.columns)}"
        )
    x = df[cols].to_numpy(dtype=np.float32)
    y = df["mos"].to_numpy(dtype=np.float32)
    if len(x) < 2:
        raise ValueError(f"split {split!r} has {len(x)} samples — need ≥2 to compute correlations")

    sess = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    pred = np.asarray(sess.run(None, {input_name: x})[0]).reshape(-1)
    if pred.shape != y.shape:
        raise ValueError(f"model output shape {pred.shape} does not match target shape {y.shape}")
    plcc = float(pearsonr(pred, y).statistic)
    srocc = float(spearmanr(pred, y).statistic)
    rmse = float(np.sqrt(((pred - y) ** 2).mean()))
    return {
        "model": str(model),
        "features": str(features),
        "split": split,
        "n": len(x),
        "plcc": plcc,
        "srocc": srocc,
        "rmse": rmse,
        "columns": cols,
    }


def _compare_models(
    models: list[Path], features: Path, split: str, input_name: str
) -> dict[str, Any]:
    """Rank @p models on the same feature split by descending PLCC."""
    reports: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []
    for m in models:
        try:
            reports.append(_eval_model_on_split(m, features, split, input_name))
        except Exception as exc:
            _logger.warning("model %s failed to evaluate; skipping", m, exc_info=True)
            errors.append({"model": str(m), "error": str(exc)})
    reports.sort(key=lambda r: r["plcc"], reverse=True)
    return {"ranked": reports, "errors": errors}


# ---------------------------------------------------------------------------
# describe_worst_frames — VLM-assisted artefact triage (ADR-0172 / T6-6)
# ---------------------------------------------------------------------------


_VLM_PROMPT = (
    "Describe what visible compression / encoding artefacts you see in this video "
    "frame in 1-2 sentences. Focus on blocking, ringing, banding, blur, or chroma "
    "distortion if present. Skip aesthetic commentary."
)

# Cached VLM pipeline (model + processor). Populated by `_load_vlm()` on
# first call and then reused. None means "unavailable / disabled".
_vlm_state: dict[str, Any] = {"loaded": False, "pipeline": None, "model_id": None}


def _load_vlm() -> tuple[Any, str] | None:
    """Lazy-import transformers and load the smallest available VLM.

    Returns ``(pipeline, model_id)`` on success, ``None`` if the
    ``vlm`` extras aren't installed or both candidate models fail to
    load. Cached across calls — the first load is slow, subsequent
    calls hit the in-memory state.
    """
    if _vlm_state["loaded"]:
        return (_vlm_state["pipeline"], _vlm_state["model_id"]) if _vlm_state["pipeline"] else None
    _vlm_state["loaded"] = True

    try:
        import torch  # noqa: F401
        from transformers import pipeline as hf_pipeline
    except ImportError:
        return None

    candidates = (
        "HuggingFaceTB/SmolVLM-Instruct",  # ~2 GB, OK on CPU
        "vikhyatk/moondream2",  # ~2 GB, well-known fallback
    )
    for model_id in candidates:
        try:
            pipe = hf_pipeline(
                "image-to-text",
                model=model_id,
                trust_remote_code=True,
            )
            _vlm_state["pipeline"] = pipe
            _vlm_state["model_id"] = model_id
            return (pipe, model_id)
        except Exception:  # pragma: no cover - depends on local env
            _logger.warning(
                "VLM candidate %s failed to load; trying next fallback",
                model_id,
                exc_info=True,
            )
            continue
    return None


def _describe_image_with_vlm(image_path: Path) -> str:
    """Run the cached VLM on @p image_path. Returns "(VLM unavailable)" when
    the ``vlm`` extras are missing or no candidate model loaded."""
    loaded = _load_vlm()
    if not loaded:
        return "(VLM unavailable — install with `pip install vmaf-mcp[vlm]`)"
    pipe, _model_id = loaded
    try:
        out = pipe(str(image_path), prompt=_VLM_PROMPT)
    except TypeError:
        # Older transformers don't accept `prompt=` for image-to-text;
        # the model defaults to its training caption prompt.
        out = pipe(str(image_path))
    if isinstance(out, list) and out and isinstance(out[0], dict):
        return str(out[0].get("generated_text") or out[0].get("text") or out[0]).strip()
    return str(out).strip()


async def _extract_frame_png(
    yuv: Path,
    *,
    width: int,
    height: int,
    pixfmt: str,
    bitdepth: int,
    frame_index: int,
    out_png: Path,
) -> None:
    """Extract a single distorted frame from a raw YUV file as PNG via
    ffmpeg. We grab a generous slice (frame_index..+1) and select the
    last frame — robust to ffmpeg's seek inaccuracy on raw YUV inputs."""
    if not shutil.which("ffmpeg"):
        raise RuntimeError("ffmpeg not on PATH; install ffmpeg to use describe_worst_frames")
    fmt_map = {
        ("420", 8): "yuv420p",
        ("422", 8): "yuv422p",
        ("444", 8): "yuv444p",
        ("420", 10): "yuv420p10le",
        ("422", 10): "yuv422p10le",
        ("444", 10): "yuv444p10le",
    }
    pix_fmt = fmt_map.get((pixfmt, bitdepth))
    if not pix_fmt:
        raise ValueError(f"unsupported pixfmt/bitdepth combo: {pixfmt}/{bitdepth}")
    argv = [
        "ffmpeg",
        "-loglevel",
        "error",
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        "-s",
        f"{width}x{height}",
        "-i",
        str(yuv),
        "-vf",
        f"select='eq(n,{frame_index})'",
        "-vsync",
        "0",
        "-frames:v",
        "1",
        "-y",
        str(out_png),
    ]
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    _stdout, stderr = await _communicate_with_timeout(proc)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg frame-extract failed: {stderr.decode(errors='replace')}")


def _pick_worst_frames(score_json: dict[str, Any], n: int) -> list[tuple[int, float]]:
    """Walk the per-frame array in @p score_json and return the @p n
    frames with lowest VMAF, sorted ascending by score.

    NaN-valued VMAF scores are skipped instead of being sorted: Python's
    ``list.sort`` is *not* a total order over NaN (NaN < x and NaN > x are
    both false), so including NaNs produced non-deterministic ranking on
    pathological inputs (e.g. a backend that emitted NaN for a partially
    decoded frame). Skipping them mirrors how a healthy VMAF run would
    treat that frame anyway: the artefact-triage caller wants real low-
    scoring frames, not undefined ones.
    """
    frames = score_json.get("frames") or []
    scored: list[tuple[int, float]] = []
    for f in frames:
        idx = f.get("frameNum")
        metrics = f.get("metrics") or {}
        # libvmaf reports the headline score under "vmaf" or "vmaf_v0.6.1"
        # (model-name-dependent). Try a few common keys.
        score = None
        for key in ("vmaf", "vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_4k_v0.6.1"):
            if key in metrics:
                raw = metrics[key]
                try:
                    score = float(raw)
                except (TypeError, ValueError):
                    score = None
                break
        if idx is None or score is None or not math.isfinite(score):
            continue
        try:
            frame_num = int(idx)
        except (TypeError, ValueError):
            # Non-numeric frameNum in the vmaf JSON output — skip this frame
            # rather than propagating a TypeError that would abort the entire
            # describe_worst_frames call.
            _logger.warning(
                "_pick_worst_frames: skipping frame with non-numeric frameNum %r",
                idx,
            )
            continue
        scored.append((frame_num, score))
    scored.sort(key=lambda kv: kv[1])
    return scored[: max(0, int(n))]


async def _describe_worst_frames(
    req: "ScoreRequest", *, n: int, describe: Any | None = None
) -> dict[str, Any]:
    """Score the pair, pick the @p n worst-VMAF frames, extract each as
    a PNG, and run the VLM (or @p describe override for tests). Returns
    a JSON-able dict: {model_id, frames: [{frame_index, vmaf, png, description}]}.
    """
    # First — score the pair so we have per-frame VMAF.
    score = await _run_vmaf_score(req)
    worst = _pick_worst_frames(score, n)

    descr_fn = describe if describe is not None else _describe_image_with_vlm
    out_frames: list[dict[str, Any]] = []
    # ADR-0608 follow-up: use a per-call unique directory rather than the
    # previous shared ``/tmp/vmaf-mcp-worst-<pid>``. Concurrent MCP tool
    # calls (e.g. two clients both asking for describe_worst_frames at the
    # same time, or one client batching N calls) raced on the shared dir:
    # the second call's ``shutil.rmtree(tmp_root)`` would delete the PNGs
    # the first call had just emitted but not yet returned to its caller.
    # ``TemporaryDirectory`` allocates a fresh atomic-O_EXCL name per call
    # (replacing the previous raw ``mkdtemp`` + ``finally: pass`` which leaked
    # the directory on every call) and cleans up automatically on context exit,
    # so callers that want persistent paths should copy the file out of the
    # response before returning.
    with tempfile.TemporaryDirectory(prefix="vmaf-mcp-worst-") as _tmp_str:
        tmp_root = Path(_tmp_str)
        for frame_idx, vmaf in worst:
            png_path = tmp_root / f"frame_{frame_idx:06d}.png"
            await _extract_frame_png(
                req.dis,
                width=req.width,
                height=req.height,
                pixfmt=req.pixfmt,
                bitdepth=req.bitdepth,
                frame_index=frame_idx,
                out_png=png_path,
            )
            description = descr_fn(png_path)
            out_frames.append(
                {
                    "frame_index": frame_idx,
                    "vmaf": vmaf,
                    "png": str(png_path),
                    "description": description,
                }
            )
    return {
        "model_id": _vlm_state.get("model_id"),
        "frames": out_frames,
    }


# NOTE (type audit, 2026-05-30): the legacy _run_benchmark() definition that
# previously lived here was a verbatim duplicate of the progress-token-aware
# implementation below.  Python silently rebound the symbol to the later
# definition at import time, but mypy correctly flagged the redefinition as a
# real bug surface — any future edit to the dead-code copy would be invisible
# at runtime.  The progress-token-aware implementation farther down is the
# single source of truth.  See ADR-0608 (progress notifications).


# ---------------------------------------------------------------------------
# list_extractors — enumerate VmafFeatureExtractor implementations (ADR-0608)
# ---------------------------------------------------------------------------

# Regex matching a top-level VmafFeatureExtractor struct definition followed
# immediately by a .name assignment.  The pattern is:
#
#   VmafFeatureExtractor vmaf_fex_<sym> = {
#       .name = "the_extractor_name",
#
# We capture the variable name (for the backend tag) and the string value.
_FEX_STRUCT_RE = re.compile(
    r"VmafFeatureExtractor\s+vmaf_fex_(\w+)\s*=\s*\{[^{]*?\.name\s*=\s*\"([^\"]+)\"",
    re.DOTALL,
)

# Backend keyword → label (longest match wins; checked against the variable
# name, not the .name string, because CUDA/SYCL/HIP twins are usually
# named ``<feature>_cuda`` etc.).
_BACKEND_KEYWORDS: list[tuple[str, str]] = [
    ("_cuda", "cuda"),
    ("_sycl", "sycl"),
    ("_hip", "hip"),
    ("_metal", "metal"),
]


def _infer_backend_from_sym(sym: str) -> str:
    """Return the backend label for a symbol like ``float_vif_cuda``."""
    for kw, label in _BACKEND_KEYWORDS:
        if sym.endswith(kw):
            return label
    return "cpu"


def _list_extractors() -> list[dict[str, Any]]:
    """Walk the libvmaf C source tree and collect every VmafFeatureExtractor
    registered in it.

    Parses only ``core/src/feature/`` (recursively) — the top-level extractor
    structs are always defined there.  Option-struct inner ``.name`` fields are
    filtered out because those structs are always preceded by the word ``Option``
    or ``VmafOption``, not ``VmafFeatureExtractor``.

    Returns a list of dicts, one per extractor, with:
    - ``name``: the C-string advertised as the extractor name (no extension).
    - ``backend``: inferred from the symbol name suffix
      (``cpu`` | ``cuda`` | ``sycl`` | ``hip`` | ``metal``).
    - ``source``: relative path to the C file that defines the struct.
    """
    feature_dir = _repo_root() / "core" / "src" / "feature"
    seen: set[tuple[str, str]] = set()
    out: list[dict[str, Any]] = []

    if not feature_dir.is_dir():
        _logger.warning(
            "list_extractors: feature directory not found at %s; is the source tree present?",
            feature_dir,
        )
        return out

    for c_file in sorted(feature_dir.rglob("*.c")):
        try:
            # Pin UTF-8 explicitly: the .c sources are ASCII in practice but
            # the default encoding inherits LC_ALL which can flip the
            # interpretation of any embedded comment bytes and break the
            # regex match below on non-UTF-8 hosts.
            text = c_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in _FEX_STRUCT_RE.finditer(text):
            sym = m.group(1)
            name = m.group(2)
            key = (sym, name)
            if key in seen:
                continue
            seen.add(key)
            out.append(
                {
                    "name": name,
                    "backend": _infer_backend_from_sym(sym),
                    "source": str(c_file.relative_to(_repo_root())),
                }
            )
    return out


# ---------------------------------------------------------------------------
# describe_model — model metadata (ADR-0608, Path.stem fix)
# ---------------------------------------------------------------------------

# Known multi-part suffixes that make Path.stem unreliable for model files.
# e.g. "vmaf_v0.6.1" → Path stem is "vmaf_v0.6" (strips ".1" as an extension).
# We strip only known media-file extensions, not arbitrary dots.
_MODEL_EXTENSIONS: frozenset[str] = frozenset({".json", ".pkl", ".onnx"})


def _strip_model_ext(filename: str) -> str:
    """Strip a known model extension from @p filename and return the stem.

    Unlike ``Path(filename).stem`` (which strips *any* ``.X`` suffix), this
    only removes extensions in ``_MODEL_EXTENSIONS``.  The bug: for
    ``vmaf_v0.6.1.json`` the stdlib would produce ``vmaf_v0.6.1`` — that's
    correct.  But for a hypothetical bare ``vmaf_v0.6.1`` query (no ext),
    ``Path('vmaf_v0.6.1').stem`` returns ``vmaf_v0.6``, dropping ``.1``.
    This function is used to normalise *registry keys* — the input is
    always the filesystem filename, not a user-supplied bare name.
    """
    ext = Path(filename).suffix
    if ext.lower() in _MODEL_EXTENSIONS:
        return filename[: -len(ext)]
    return filename


def _describe_model(name_or_path: str) -> dict[str, Any]:
    """Return metadata for the VMAF model identified by @p name_or_path.

    Resolution order:
    1. Treat @p name_or_path as an exact filesystem path (absolute or
       relative to repo root).  If it exists and is a recognised model
       file, describe it directly.
    2. Match against every model file in ``model/`` (recursively) by
       comparing the full filename (no extension) against @p name_or_path.
       This avoids the ``Path.stem`` bug where ``vmaf_v0.6.1`` would be
       matched as ``vmaf_v0.6`` by stdlib stem logic.

    Returned dict:
    - ``name``: filename stem (extension stripped).
    - ``path``: repo-relative path.
    - ``format``: ``json`` / ``pkl`` / ``onnx``.
    - ``size_bytes``: file size.
    - ``model_type``: for JSON models, the ``model_dict.model_type`` value
      (e.g. ``LIBSVMNUSVR``); ``null`` for ONNX / PKL.
    - ``feature_names``: for JSON models, the ``model_dict.feature_names``
      list; ``null`` otherwise.
    """
    repo = _repo_root()
    models_dir = repo / "model"

    # --- Step 1: try as a direct path ---
    candidate = Path(name_or_path)
    if not candidate.is_absolute():
        candidate = repo / candidate
    candidate = candidate.resolve()
    if candidate.is_file() and candidate.suffix.lower() in _MODEL_EXTENSIONS:
        # Explicit allowlist guard — mirrors _validate_path() to make the
        # security invariant unconditional rather than relying on the model/
        # directory being under an allowlisted root by construction.
        allowed = _allowed_roots()
        if not any(candidate.is_relative_to(r) for r in allowed):
            raise ValueError(
                f"model path {candidate} not under an allowlisted root; "
                "set VMAF_MCP_ALLOW to extend."
            )
        return _describe_model_file(candidate, repo)

    # --- Step 2: search by filename match (full name, no extension) ---
    # Build index keyed by EXACT filename (no ext) for unambiguous lookup.
    # Use _strip_model_ext (not Path.stem) so "vmaf_v0.6.1" matches
    # "vmaf_v0.6.1.json" correctly.
    matches: list[Path] = []
    for p in models_dir.rglob("*"):
        if p.suffix.lower() not in _MODEL_EXTENSIONS or not p.is_file():
            continue
        stem = _strip_model_ext(p.name)
        if stem == name_or_path or p.name == name_or_path:
            matches.append(p)

    if not matches:
        raise ValueError(
            f"model {name_or_path!r} not found; run list_models to see available models."
        )
    if len(matches) > 1:
        paths = [str(m.relative_to(repo)) for m in matches]
        raise ValueError(
            f"model name {name_or_path!r} is ambiguous; matched: {paths}. "
            "Pass an explicit path instead."
        )
    return _describe_model_file(matches[0], repo)


def _describe_model_file(path: Path, repo: Path) -> dict[str, Any]:
    """Build the metadata dict for @p path (already resolved and verified)."""
    stat = path.stat()
    ext = path.suffix.lower().lstrip(".")
    stem = _strip_model_ext(path.name)
    result: dict[str, Any] = {
        "name": stem,
        "path": str(path.relative_to(repo)),
        "format": ext,
        "size_bytes": stat.st_size,
        "model_type": None,
        "feature_names": None,
    }

    # Parse JSON models for richer metadata.
    if ext == "json":
        try:
            payload = json.loads(path.read_text(encoding="utf-8", errors="replace"))
            model_dict = payload.get("model_dict") or {}
            result["model_type"] = model_dict.get("model_type")
            result["feature_names"] = model_dict.get("feature_names")
        except (json.JSONDecodeError, OSError):
            # Malformed or unreadable JSON — fall back to the partial metadata
            # already accumulated above (name, path, format, size_bytes).
            pass

    return result


# ---------------------------------------------------------------------------
# _vmaftune_binary — resolve the vmaf-tune CLI (ADR-0608)
# ---------------------------------------------------------------------------


def _vmaftune_binary() -> Path:
    """Return the path to the vmaf-tune CLI binary.

    Resolution order:
    1. ``VMAF_TUNE_BIN`` environment variable (explicit override).
    2. ``vmaf-tune`` on ``PATH`` (installed wheel or editable install).
    3. ``<repo>/tools/vmaf-tune/vmaf-tune`` — in-tree wrapper script.

    Returns the first candidate that exists.  If none exist the last
    candidate is returned so callers get a clear "not found" error.
    """
    env = os.environ.get("VMAF_TUNE_BIN")
    if env:
        return Path(env)

    which_result = shutil.which("vmaf-tune")
    if which_result:
        return Path(which_result)

    # Fall back to the in-tree wrapper script shipped with the repo.
    fallback = _repo_root() / "tools" / "vmaf-tune" / "vmaf-tune"
    return fallback


# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# _send_progress — MCP progress notification helper (ADR-0608)
# ---------------------------------------------------------------------------


async def _send_progress(
    progress_token: str | int | None,
    progress: float,
    total: float,
    message: str,
) -> None:
    """Emit a ``notifications/progress`` notification if the client supplied
    a ``_meta.progressToken`` in the tool-call params.

    Per the MCP spec the client opts in by passing ``progressToken`` in the
    ``params._meta`` object of the ``tools/call`` request.  The server MUST
    NOT send progress if no token was provided (the client has no handler).

    Implementation note: ``server.request_context.session`` is a
    ``ServerSession`` with ``send_progress_notification`` available in
    mcp>=1.0.  We guard against ``LookupError`` so callers outside a
    request context (tests) do not fail.
    """
    if progress_token is None:
        return
    try:
        await server.request_context.session.send_progress_notification(
            progress_token=progress_token,
            progress=progress,
            total=total,
            message=message,
        )
    except LookupError:
        # Called outside a request context (e.g. unit tests) — silently skip.
        pass
    except Exception:
        # Progress notifications are best-effort; a failure here must NEVER
        # propagate and abort the tool call. Log the cause so operators can
        # spot a misbehaving MCP transport without re-triggering the run.
        _logger.warning(
            "send_progress_notification failed; suppressing to keep tool call alive",
            exc_info=True,
        )


# ---------------------------------------------------------------------------
# run_compare — wraps vmaf-tune compare (ADR-0608)
# ---------------------------------------------------------------------------


async def _run_compare(
    src: str,
    *,
    target_vmaf: float | None = None,
    target_vmafs: str | None = None,
    encoders: str | None = None,
    format: str = "json",
    width: int | None = None,
    height: int | None = None,
    pix_fmt: str = "yuv420p",
    framerate: float | None = None,
    no_parallel: bool = False,
    progress_token: str | int | None = None,
) -> dict[str, Any]:
    """Run ``vmaf-tune compare`` and return the parsed report.

    @p src must be a local file path under an allowlisted root (validated via
    :func:`_validate_media_path`).  Remote URL schemes (http, rtsp, s3, etc.)
    are rejected to prevent SSRF via ffmpeg's URL demuxer.

    Progress notifications are emitted at "started" and "done" when
    @p progress_token is set (clients opt in via ``params._meta.progressToken``).
    vmaf-tune compare typically runs 30 s–3 min depending on the number of
    encoders and the source duration; no finer-grained progress is available
    from the subprocess.
    """
    src = _validate_media_path(src)
    vmaftune = _vmaftune_binary()
    if not vmaftune.exists():
        raise RuntimeError(
            f"vmaf-tune binary not found at {vmaftune}. "
            "Install with: pip install -e tools/vmaf-tune or set VMAF_TUNE_BIN."
        )

    await _send_progress(progress_token, 0.0, 1.0, "starting vmaf-tune compare")

    argv: list[str] = [str(vmaftune), "compare", "--src", src, "--format", format]
    if target_vmafs is not None:
        argv += ["--target-vmafs", target_vmafs]
    elif target_vmaf is not None:
        argv += ["--target-vmaf", str(target_vmaf)]
    if encoders is not None:
        argv += ["--encoders", encoders]
    if width is not None:
        argv += ["--width", str(width)]
    if height is not None:
        argv += ["--height", str(height)]
    argv += ["--pix-fmt", pix_fmt]
    if framerate is not None:
        argv += ["--framerate", str(framerate)]
    if no_parallel:
        argv.append("--no-parallel")

    proc = await asyncio.create_subprocess_exec(
        *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    stdout, stderr = await _communicate_with_timeout(proc)

    await _send_progress(progress_token, 1.0, 1.0, "vmaf-tune compare done")

    stdout_s = stdout.decode(errors="replace")
    stderr_s = stderr.decode(errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"vmaf-tune compare exited {proc.returncode}: {stderr_s.strip()}")

    # vmaf-tune compare --format json emits JSON to stdout; other formats
    # return a string.  We always pass --format json here so we can parse it.
    try:
        parsed: dict[str, Any] = json.loads(stdout_s)
        return parsed
    except json.JSONDecodeError:
        # Non-JSON format or parse error — return raw output.
        return {
            "exit_code": proc.returncode,
            "stdout": stdout_s,
            "stderr": stderr_s,
        }


# ---------------------------------------------------------------------------
# run_ladder — wraps vmaf-tune ladder (ADR-0608)
# ---------------------------------------------------------------------------


async def _run_ladder(
    src: str,
    resolutions: str,
    target_vmafs: str,
    *,
    encoder: str = "libx264",
    quality_tiers: int = 5,
    format: str = "json",
    spacing: str = "log_bitrate",
    framerate: float | None = None,
    progress_token: str | int | None = None,
) -> dict[str, Any]:
    """Run ``vmaf-tune ladder`` and return the manifest.

    @p src must be a local file path under an allowlisted root (validated via
    :func:`_validate_media_path`).
    @p resolutions: comma-separated ``WxH`` list, e.g. ``1920x1080,1280x720``.
    @p target_vmafs: comma-separated VMAF target list, e.g. ``95,90,85``.
    """
    src = _validate_media_path(src)
    vmaftune = _vmaftune_binary()
    if not vmaftune.exists():
        raise RuntimeError(
            f"vmaf-tune binary not found at {vmaftune}. "
            "Install with: pip install -e tools/vmaf-tune or set VMAF_TUNE_BIN."
        )

    await _send_progress(progress_token, 0.0, 1.0, "starting vmaf-tune ladder")

    argv: list[str] = [
        str(vmaftune),
        "ladder",
        "--src",
        src,
        "--encoder",
        encoder,
        "--resolutions",
        resolutions,
        "--target-vmafs",
        target_vmafs,
        "--quality-tiers",
        str(quality_tiers),
        "--format",
        format,
        "--spacing",
        spacing,
    ]
    if framerate is not None:
        argv += ["--framerate", str(framerate)]

    proc = await asyncio.create_subprocess_exec(
        *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    stdout, stderr = await _communicate_with_timeout(proc)

    await _send_progress(progress_token, 1.0, 1.0, "vmaf-tune ladder done")

    stdout_s = stdout.decode(errors="replace")
    stderr_s = stderr.decode(errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"vmaf-tune ladder exited {proc.returncode}: {stderr_s.strip()}")

    # --format json emits JSON to stdout; HLS/DASH returns a manifest string.
    if format == "json":
        try:
            return {"manifest": json.loads(stdout_s), "format": format}
        except json.JSONDecodeError:
            # vmaf-tune unexpectedly emitted non-JSON despite --format json;
            # fall through and return the raw stdout as a plain manifest string.
            pass
    return {"manifest": stdout_s, "format": format}


# ---------------------------------------------------------------------------
# run_tune_per_shot — wraps vmaf-tune tune-per-shot (ADR-0608)
# ---------------------------------------------------------------------------


async def _run_tune_per_shot(
    src: str,
    *,
    target_vmaf: float = 92.0,
    encoder: str = "libx264",
    output: str | None = None,
    pix_fmt: str = "yuv420p",
    framerate: float | None = None,
    scene_threshold: float | None = None,
    format: str = "json",
    progress_token: str | int | None = None,
) -> dict[str, Any]:
    """Run ``vmaf-tune tune-per-shot`` and return per-shot recommendations.

    @p src must be a local file path under an allowlisted root (validated via
    :func:`_validate_media_path`).  Detects scene cuts in the source, runs a
    per-shot CRF bisect targeting @p target_vmaf with @p encoder, and returns
    the plan.

    Progress notifications: emitted at start and completion (two steps);
    the actual bisect may take minutes for a long clip.
    """
    src = _validate_media_path(src)
    vmaftune = _vmaftune_binary()
    if not vmaftune.exists():
        raise RuntimeError(
            f"vmaf-tune binary not found at {vmaftune}. "
            "Install with: pip install -e tools/vmaf-tune or set VMAF_TUNE_BIN."
        )

    await _send_progress(progress_token, 0.0, 1.0, "starting vmaf-tune tune-per-shot")

    # Note: vmaf-tune tune-per-shot does not accept a ``--format`` flag;
    # it always writes the JSON plan to stdout.  The ``format`` parameter
    # is accepted by the MCP tool schema for forward-compatibility but is
    # ignored here — stdout is always parsed as JSON.
    argv: list[str] = [
        str(vmaftune),
        "tune-per-shot",
        "--src",
        src,
        "--target-vmaf",
        str(target_vmaf),
        "--encoder",
        encoder,
        "--pix-fmt",
        pix_fmt,
    ]
    if output is not None:
        argv += ["--output", output]
    if framerate is not None:
        argv += ["--framerate", str(framerate)]
    if scene_threshold is not None:
        argv += ["--scene-threshold", str(scene_threshold)]

    proc = await asyncio.create_subprocess_exec(
        *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    stdout, stderr = await _communicate_with_timeout(proc)

    await _send_progress(progress_token, 1.0, 1.0, "vmaf-tune tune-per-shot done")

    stdout_s = stdout.decode(errors="replace")
    stderr_s = stderr.decode(errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"vmaf-tune tune-per-shot exited {proc.returncode}: {stderr_s.strip()}")

    try:
        parsed: dict[str, Any] = json.loads(stdout_s)
        return parsed
    except json.JSONDecodeError:
        # vmaf-tune emitted non-JSON stdout; return the raw output so the
        # caller can inspect it.
        return {"exit_code": proc.returncode, "stdout": stdout_s, "stderr": stderr_s}


# ---------------------------------------------------------------------------
# run_benchmark (patched to emit progress notifications — ADR-0608)
# ---------------------------------------------------------------------------


async def _run_benchmark(
    progress_token: str | int | None = None,
) -> dict[str, Any]:
    """Run the full multi-fixture benchmark suite (bench_all.sh).

    bench_all.sh is a fixed-fixture harness: it tests three canonical YUV
    pairs (576x324, 1080p-5f, 4K-BBB-200f) across all available backends.
    It does NOT accept per-call ref/dis arguments — those are hardcoded.

    Root causes of the original failure (ADR-0513):
    1. The MCP tool previously passed ``-r ref -d dis --width W --height H``
       as positional args to the script.  bench_all.sh uses ``set -euo
       pipefail`` and sources Intel oneAPI ``setvars.sh`` inside the script
       body.  ``setvars.sh`` reads the calling script's ``$@`` (positional
       parameters) to process its own flags; the unknown flags ``-r``,
       ``-d``, ``--width``, ``--height`` propagate into per-component
       ``env/vars.sh`` scripts, which hang or exit non-zero, aborting the
       outer script before any output is emitted.
    2. ``VMAF_BIN`` was not injected into the subprocess environment, so the
       script fell back to the relative path ``core/build/tools/vmaf``
       which is absent in the container after ``make install``.

    Progress notifications (ADR-0608): emitted at start and completion.
    The benchmark takes 30–120 s; no finer-grained progress is available
    from the shell script.
    """
    script = _repo_root() / "testdata" / "bench_all.sh"
    if not script.exists():
        raise FileNotFoundError(f"benchmark harness not found: {script}")
    # Resolve the data root — where bench_all.sh's fixture YUVs live.
    # Priority:
    #   1. VMAF_ROOT env var already set by the caller (explicit override).
    #   2. _repo_root() when it contains the canonical fixture file.
    #   3. /workspace — the vmaf-dev-mcp container bind-mount (ADR-0513).
    # This handles the case where the MCP server is installed as an editable
    # package from a git worktree (e.g. during development): _repo_root()
    # then resolves to the worktree directory which shares the git objects but
    # does not have the large YUV fixtures checked out.
    _fixture_probe = "python/test/resource/yuv/src01_hrc00_576x324.yuv"
    _candidate_roots = [
        Path(os.environ["VMAF_ROOT"]) if "VMAF_ROOT" in os.environ else None,
        _repo_root(),
        Path("/workspace"),
    ]
    vmaf_root = next(
        (r for r in _candidate_roots if r is not None and (r / _fixture_probe).exists()),
        _repo_root(),
    )
    # Inherit the full environment so that PATH, LD_LIBRARY_PATH, and any
    # GPU-runtime variables are preserved.  Inject VMAF_ROOT so bench_all.sh
    # resolves its ``cd`` correctly when git is unavailable, and inject
    # VMAF_BIN so the script uses the installed binary (not the relative
    # in-tree path which is absent after ``make install`` in containers).
    bench_env = {
        **os.environ,
        "VMAF_ROOT": str(vmaf_root),
        "VMAF_BIN": str(_vmaf_binary()),
    }

    await _send_progress(progress_token, 0.0, 1.0, "starting benchmark harness")

    # bench_all.sh is invoked with NO positional arguments.  The script's
    # fixture paths are hardcoded; passing extra args corrupts $@ inside
    # the sourced oneAPI setvars.sh and causes a silent abort (ADR-0517).
    proc = await asyncio.create_subprocess_exec(
        str(script),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=bench_env,
    )
    stdout, stderr = await _communicate_with_timeout(proc)

    await _send_progress(progress_token, 1.0, 1.0, "benchmark harness done")

    stdout_s = stdout.decode(errors="replace")
    stderr_s = stderr.decode(errors="replace")
    # ADR-0608 E-1: previously, a non-zero exit from bench_all.sh returned a
    # success-shaped payload that the MCP layer wrapped with isError=False —
    # callers could not branch on "benchmark failed".  Raise so the MCP
    # surface marks the call as isError=True (matches the sibling
    # _run_compare / _run_ladder / _run_tune_per_shot error path).
    if proc.returncode != 0:
        detail = stderr_s.strip() or stdout_s.strip()
        if not detail:
            detail = (
                "no output — likely aborted by set -euo pipefail before printing. "
                f"Common causes: missing vmaf binary at {_vmaf_binary()}, missing "
                "fixture YUVs under testdata/ or python/test/resource/yuv/. "
                "Re-run with `bash -x testdata/bench_all.sh` to bisect."
            )
        raise RuntimeError(f"benchmark failed (rc={proc.returncode}): {detail}")
    payload: dict[str, Any] = {
        "exit_code": proc.returncode,
        "stdout": stdout_s,
        "stderr": stderr_s,
    }
    return payload


# probe_backend — runtime health check (ADR-0608 / C-P0-1)
# ---------------------------------------------------------------------------

# Minimal 64×64 1-frame 4:2:0 8-bit YUV filled with mid-grey (Y=128, Cb=Cr=128).
# 64×64 × 1.5 = 6144 bytes.  Tiny enough to decode in milliseconds on any backend.
# 32×32 was insufficient for CUDA ADM which requires at least 36px in each
# dimension; a 32px frame silently returned a null score from the ADM kernel,
# which the old code misreported as runtime_healthy=True (ADR-0608 follow-up).
_PROBE_YUV_WIDTH = 64
_PROBE_YUV_HEIGHT = 64
_PROBE_YUV_BYTES = _PROBE_YUV_WIDTH * _PROBE_YUV_HEIGHT * 3 // 2  # 6144 for 4:2:0 8-bit
_PROBE_YUV_DATA = bytes([128]) * _PROBE_YUV_BYTES


async def _probe_backend(backend: str) -> dict[str, Any]:
    """Run a 1-frame VMAF score with @p backend and return a health dict.

    Schema: ``{backend, compiled_in, runtime_healthy, latency_ms, score, error}``.
    ``compiled_in`` reflects whether the local binary advertises the backend.
    ``runtime_healthy`` is ``True`` iff the subprocess exits 0 and returns a
    finite score, ``False`` otherwise (driver absent, ICD missing, KFD ioctl
    failure, etc.).
    """
    vmaf = _vmaf_binary()
    # ADR-1023: use async wrapper to avoid blocking the event loop on first probe.
    compiled_in = backend == "cpu" or backend in await _probe_backends_async(vmaf)

    if not compiled_in:
        return {
            "backend": backend,
            "compiled_in": False,
            "runtime_healthy": False,
            "latency_ms": None,
            "score": None,
            "error": f"backend {backend!r} is not compiled into the local vmaf binary",
        }

    import tempfile
    import time

    with tempfile.TemporaryDirectory(prefix="vmaf-mcp-probe-") as tmp:
        tmp_path = Path(tmp)
        ref_yuv = tmp_path / "ref.yuv"
        dis_yuv = tmp_path / "dis.yuv"
        out_json = tmp_path / "out.json"
        ref_yuv.write_bytes(_PROBE_YUV_DATA)
        dis_yuv.write_bytes(_PROBE_YUV_DATA)

        argv = [
            str(vmaf),
            "-r",
            str(ref_yuv),
            "-d",
            str(dis_yuv),
            "--width",
            str(_PROBE_YUV_WIDTH),
            "--height",
            str(_PROBE_YUV_HEIGHT),
            "-p",
            "420",
            "-b",
            "8",
            "-m",
            "version=vmaf_v0.6.1",
            "--precision",
            "legacy",  # %.6f — matches C CLI default (ADR-0119)
            "-q",
            "-o",
            str(out_json),
            "--json",
        ]
        if backend in _BACKEND_DISABLE:
            for sibling in _BACKEND_DISABLE[backend]:
                argv.append(f"--no_{sibling}")

        t0 = time.monotonic()
        try:
            proc = await asyncio.create_subprocess_exec(
                *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            _stdout, stderr = await _communicate_with_timeout(proc)
        except OSError as exc:
            return {
                "backend": backend,
                "compiled_in": compiled_in,
                "runtime_healthy": False,
                "latency_ms": None,
                "score": None,
                "error": f"failed to exec vmaf: {exc}",
            }
        latency_ms = (time.monotonic() - t0) * 1000.0

        if proc.returncode != 0:
            return {
                "backend": backend,
                "compiled_in": compiled_in,
                "runtime_healthy": False,
                "latency_ms": round(latency_ms, 1),
                "score": None,
                "error": f"vmaf exited {proc.returncode}: {stderr.decode(errors='replace').strip()[:500]}",
            }

        try:
            payload = json.loads(out_json.read_text(encoding="utf-8"))
            pooled = payload.get("pooled_metrics") or {}
            vmaf_pool = pooled.get("vmaf") or {}
            score = vmaf_pool.get("mean")
        except Exception as exc:
            _logger.warning("probe %s: failed to parse vmaf JSON output", backend, exc_info=True)
            return {
                "backend": backend,
                "compiled_in": compiled_in,
                "runtime_healthy": False,
                "latency_ms": round(latency_ms, 1),
                "score": None,
                "error": f"failed to parse vmaf output: {exc}",
            }

        # runtime_healthy requires a non-null score: a null score indicates
        # the backend kernel failed silently (e.g. ADM sub-minimum resolution,
        # driver absent) even though the process returned exit code 0.
        return {
            "backend": backend,
            "compiled_in": compiled_in,
            "runtime_healthy": score is not None,
            "latency_ms": round(latency_ms, 1),
            "score": score,
            "error": None if score is not None else "vmaf returned exit 0 but score was null",
        }


# ---------------------------------------------------------------------------
# vmaf_version — binary identity + build flags (ADR-0608 / C-P0-3)
# ---------------------------------------------------------------------------


async def _vmaf_version() -> dict[str, Any]:
    """Return the local vmaf binary's version string and compiled backends.

    Runs ``vmaf --version`` (for the version string) and ``vmaf --help``
    (for the backend compile-in flags, same probe used by ``_probe_backends``).
    The ``--version`` banner does not reliably list backends (Bug A, ADR-0511),
    so we derive ``build_flags`` from ``--help`` instead.

    ADR-1023: this function is async so the blocking ``subprocess.run``
    (``--version``) and ``_probe_backends_async`` (``--help``) do not stall
    the event loop when called from the async ``_call_tool`` handler.
    """
    vmaf = _vmaf_binary()
    binary_path = str(vmaf)

    if not vmaf.exists():
        return {
            "binary_path": binary_path,
            "version": None,
            "build_flags": {
                "cpu": False,
                "cuda": False,
                "sycl": False,
                "hip": False,
                "metal": False,
            },
            "error": f"vmaf binary not found at {binary_path}",
        }

    # --- version string (blocking subprocess.run, run in thread) ---
    def _get_version() -> str | None:
        try:
            result = subprocess.run(
                [str(vmaf), "--version"],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            blob = (result.stdout or "") + (result.stderr or "")
            # The banner looks like "vmaf 3.0.0-lusoris.5" or just "3.0.0".
            m = re.search(r"(\d+\.\d+[\w.\-]*)", blob)
            return m.group(1) if m else blob.strip().splitlines()[0] if blob.strip() else None
        except (subprocess.TimeoutExpired, OSError):
            return None  # Binary present but --version timed out; fall through.

    version_str = await asyncio.to_thread(_get_version)

    # --- build flags from --help (same as _probe_backends but we don't use the cache
    #     so callers always get a fresh view even when the binary changes after startup) ---
    advertised = await _probe_backends_async(vmaf)
    build_flags = {
        "cpu": True,  # CPU is always compiled in when the binary exists.
        "cuda": "cuda" in advertised,
        "sycl": "sycl" in advertised,
        "hip": "hip" in advertised,
        "metal": "metal" in advertised,
    }

    return {
        "binary_path": binary_path,
        "version": version_str,
        "build_flags": build_flags,
        "error": None,
    }


# ---------------------------------------------------------------------------
# vmaf_score_encoded — decode encoded video then score (ADR-0608 / C-P0-2)
# ---------------------------------------------------------------------------

# ffmpeg pixel-format mapping used for the decoded YUV temp files.
_PIXFMT_TO_FFMPEG: dict[tuple[str, int], str] = {
    ("420", 8): "yuv420p",
    ("422", 8): "yuv422p",
    ("444", 8): "yuv444p",
    ("420", 10): "yuv420p10le",
    ("422", 10): "yuv422p10le",
    ("444", 10): "yuv444p10le",
    ("420", 12): "yuv420p12le",
    ("422", 12): "yuv422p12le",
    ("444", 12): "yuv444p12le",
}


def _ffprobe_geometry(path: Path) -> tuple[int, int, str, int]:
    """Return ``(width, height, pixfmt, bitdepth)`` for the first video stream
    in @p path.  ``pixfmt`` is ``"420"`` / ``"422"`` / ``"444"``.

    Raises :class:`RuntimeError` when ffprobe is unavailable or the probe
    fails, and :class:`ValueError` when the stream has no video or the pixel
    format cannot be mapped.
    """
    if not shutil.which("ffprobe"):
        raise RuntimeError("ffprobe not on PATH; install ffmpeg to use vmaf_score_encoded")

    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "quiet",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,pix_fmt",
            "-of",
            "json",
            str(path),
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"ffprobe failed (rc={result.returncode}): {result.stderr.strip()[:300]}"
        )
    try:
        info = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"ffprobe output malformed (empty stdout or no video track?): {exc}"
        ) from exc
    streams = info.get("streams") or []
    if not streams:
        raise ValueError(f"no video stream found in {path}")
    s = streams[0]
    # ffprobe can return a v:0 stream lacking width/height (e.g. attached-pic
    # cover art or data-coded video tracks). Validate explicitly so the
    # docstring's ValueError contract holds, rather than leaking a KeyError.
    width_raw = s.get("width")
    height_raw = s.get("height")
    if width_raw is None or height_raw is None:
        raise ValueError(f"video stream in {path} has no width/height")
    width = int(width_raw)
    height = int(height_raw)
    pix_fmt_raw = str(s.get("pix_fmt", "yuv420p"))

    # Map ffmpeg pix_fmt names to the vmaf (pixfmt, bitdepth) pair.
    _map: dict[str, tuple[str, int]] = {
        "yuv420p": ("420", 8),
        "yuvj420p": ("420", 8),
        "yuv422p": ("422", 8),
        "yuvj422p": ("422", 8),
        "yuv444p": ("444", 8),
        "yuvj444p": ("444", 8),
        "yuv420p10le": ("420", 10),
        "yuv422p10le": ("422", 10),
        "yuv444p10le": ("444", 10),
        "yuv420p12le": ("420", 12),
        "yuv422p12le": ("422", 12),
        "yuv444p12le": ("444", 12),
        "yuv420p10be": ("420", 10),
        "yuv422p10be": ("422", 10),
        "yuv444p10be": ("444", 10),
    }
    mapped = _map.get(pix_fmt_raw)
    if mapped is None:
        raise ValueError(
            f"pixel format {pix_fmt_raw!r} cannot be mapped to a vmaf pixfmt/bitdepth; "
            "supported: yuv420p/yuv422p/yuv444p (8/10/12-bit)"
        )
    pixfmt, bitdepth = mapped
    return width, height, pixfmt, bitdepth


async def _ffprobe_geometry_async(path: Path) -> tuple[int, int, str, int]:
    """Async-safe wrapper around :func:`_ffprobe_geometry` (ADR-1023).

    Runs the blocking ``subprocess.run`` inside :func:`_ffprobe_geometry`
    in a thread-pool worker so the event loop is not stalled while
    waiting for ffprobe to complete.
    """
    return await asyncio.to_thread(_ffprobe_geometry, path)


async def _decode_to_yuv(src: Path, dst: Path, *, pix_fmt: str) -> None:
    """Decode the encoded video at @p src to a raw YUV file at @p dst via
    ffmpeg.  @p pix_fmt is the ``yuv420p``-style ffmpeg format string.

    Raises :class:`RuntimeError` on ffmpeg failure.
    """
    if not shutil.which("ffmpeg"):
        raise RuntimeError("ffmpeg not on PATH; install ffmpeg to use vmaf_score_encoded")

    argv = [
        "ffmpeg",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        "-y",
        str(dst),
    ]
    proc = await asyncio.create_subprocess_exec(
        *argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    _stdout, stderr = await _communicate_with_timeout(proc)
    if proc.returncode != 0:
        raise RuntimeError(
            f"ffmpeg decode failed (rc={proc.returncode}): {stderr.decode(errors='replace').strip()[:500]}"
        )


async def _run_vmaf_score_encoded(
    ref_path: Path,
    dis_path: Path,
    *,
    model: str = "version=vmaf_v0.6.1",
    backend: str = "auto",
    subsample: int = 1,
    precision: str = "legacy",  # "legacy" = %.6f; matches C CLI default per ADR-0119,
    extras: ScoreExtras | None = None,  # optional pass-through flags (ADR-1117)
) -> dict[str, Any]:
    """Decode both encoded inputs to temp raw YUV, then delegate to
    :func:`_run_vmaf_score`.

    Geometry is probed from the reference stream; both reference and distorted
    must have the same dimensions.  ``subsample`` passes ``--subsample`` to
    vmaf (1 = every frame).  ``extras`` forwards the optional ADR-1117
    pass-through scoring flags.
    """
    import tempfile

    if not shutil.which("ffprobe"):
        raise RuntimeError("ffprobe not on PATH; install ffmpeg to use vmaf_score_encoded")

    # ADR-1023: use the async wrapper so the blocking subprocess.run inside
    # _ffprobe_geometry does not stall the event loop.
    width, height, pixfmt, bitdepth = await _ffprobe_geometry_async(ref_path)
    ffmpeg_pix_fmt = _PIXFMT_TO_FFMPEG.get((pixfmt, bitdepth))
    if ffmpeg_pix_fmt is None:
        raise ValueError(f"unsupported pixfmt/bitdepth combination: {pixfmt}/{bitdepth}")

    with tempfile.TemporaryDirectory(prefix="vmaf-mcp-encoded-") as tmp:
        tmp_p = Path(tmp)
        ref_yuv = tmp_p / "ref.yuv"
        dis_yuv = tmp_p / "dis.yuv"

        # Decode both inputs in parallel for speed.
        # ADR-1023: return_exceptions=True so one failing decode does not
        # silently swallow the other's error.  We inspect both results and
        # re-raise the first exception found so the caller gets a clear message.
        decode_results = await asyncio.gather(
            _decode_to_yuv(ref_path, ref_yuv, pix_fmt=ffmpeg_pix_fmt),
            _decode_to_yuv(dis_path, dis_yuv, pix_fmt=ffmpeg_pix_fmt),
            return_exceptions=True,
        )
        for _res in decode_results:
            if isinstance(_res, BaseException):
                raise _res

        req = ScoreRequest(
            ref=ref_yuv,
            dis=dis_yuv,
            width=width,
            height=height,
            pixfmt=pixfmt,
            bitdepth=bitdepth,
            model=model,
            backend=backend,
            precision=precision,
            subsample=subsample,
            extras=extras if extras is not None else ScoreExtras(),
        )
        result = await _run_vmaf_score(req)
        # Surface the original encoded paths in the response.
        result["reference_encoded"] = str(ref_path)
        result["distorted_encoded"] = str(dis_path)
        return result


# ---------------------------------------------------------------------------


# MCP server wiring
# ---------------------------------------------------------------------------


server: Server = Server("vmaf-mcp")


# mcp.server.lowlevel.Server.list_tools() returns an untyped decorator (the
# library has no py.typed marker); this is a library-stub gap, not a real
# typing issue at the call site.
def _scoring_extra_properties() -> dict[str, Any]:
    """Return the optional pass-through scoring parameters shared by
    ``vmaf_score`` and ``vmaf_score_encoded`` (ADR-1117).

    Each property maps onto a ``vmaf`` CLI flag verified against
    ``core/tools/cli_parse.c``. Every property is optional and only forwarded
    to the CLI when the caller supplies it, so existing callers are unaffected.

    MUST stay byte-identical to the Go server's ``scoringExtraProperties()``
    (cmd/vmafx-mcp/tools.go) — same keys, enums, defaults, and descriptions —
    per cmd/vmafx-mcp/AGENTS.md.
    """
    return {
        # --- Feature selection + CTC presets ---
        "feature": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Additional feature extractors, each passed as a repeated "
            "--feature flag. Use the libvmaf 'name[=key=val:...]' syntax, e.g. "
            "'psnr' or 'cambi=full_ref=true'. Mutually exclusive with aom_ctc/nflx_ctc.",
        },
        "aom_ctc": {
            "type": "string",
            "enum": ["v1.0", "v2.0", "v3.0", "v4.0", "v5.0", "v6.0", "v7.0"],
            "description": "AOM Common Test Conditions preset (--aom_ctc). Configures a fixed "
            "model + feature set; mutually exclusive with manual feature/model config.",
        },
        "nflx_ctc": {
            "type": "string",
            "enum": ["v1.0"],
            "description": "Netflix Common Test Conditions preset (--nflx_ctc). Mutually "
            "exclusive with manual feature/model config.",
        },
        # --- Tiny-AI / DNN scoring surface (ADR-1117) ---
        "tiny_model": {
            "type": "string",
            "description": "Path to a tiny ONNX model loaded alongside classic models (--tiny-model).",
        },
        "tiny_device": {
            "type": "string",
            "enum": [
                "auto",
                "cpu",
                "cuda",
                "openvino",
                "openvino-npu",
                "openvino-cpu",
                "openvino-gpu",
                "coreml",
                "coreml-ane",
                "coreml-gpu",
                "coreml-cpu",
                "rocm",
            ],
            "description": "ONNX Runtime execution provider for the tiny model (--tiny-device / "
            "--dnn-ep). Default: auto.",
        },
        "tiny_threads": {
            "type": "integer",
            "minimum": 0,
            "description": "CPU EP intra-op thread count for the tiny model (--tiny-threads; 0 = ORT default).",
        },
        "tiny_fp16": {
            "type": "boolean",
            "description": "Request fp16 IO where the execution provider supports it (--tiny-fp16).",
        },
        "tiny_model_verify": {
            "type": "boolean",
            "description": "Require Sigstore-bundle verification of the tiny model before use (--tiny-model-verify).",
        },
        "tiny_codec": {
            "type": "string",
            "description": "Encoder name for codec-aware tiny models (--tiny-codec), e.g. libx264.",
        },
        "tiny_preset": {
            "type": "string",
            "description": "Encoder preset string for codec-aware tiny models (--tiny-preset).",
        },
        "tiny_crf": {
            "type": "integer",
            "minimum": 0,
            "maximum": 63,
            "description": "CRF / QP integer for codec-aware tiny models (--tiny-crf; clamped to 0..63).",
        },
        "tiny_resize": {
            "type": "string",
            "enum": ["bilinear", "nearest", "bicubic", "disabled"],
            "description": "Auto-resize filter for NCHW tiny models on dimension mismatch "
            "(--tiny-resize). Default: disabled (mismatch hard-errors).",
        },
        "no_reference": {
            "type": "boolean",
            "description": "No-reference (NR) mode (--no-reference). Requires tiny_model (an NR "
            "ONNX model); the reference path becomes a formality — pass any valid YUV "
            "of matching geometry since only the distorted picture is scored.",
        },
        # --- Score-param completeness ---
        "threads": {
            "type": "integer",
            "minimum": 1,
            "description": "Worker thread count (--threads). Capped to hardware cores by the CLI.",
        },
        "frame_cnt": {
            "type": "integer",
            "minimum": 1,
            "description": "Maximum number of frames to process (--frame_cnt).",
        },
        "frame_skip_ref": {
            "type": "integer",
            "minimum": 0,
            "description": "Skip the first N frames of the reference (--frame_skip_ref).",
        },
        "frame_skip_dist": {
            "type": "integer",
            "minimum": 0,
            "description": "Skip the first N frames of the distorted input (--frame_skip_dist).",
        },
        "no_prediction": {
            "type": "boolean",
            "description": "Extract features only, skip VMAF prediction (--no_prediction).",
        },
    }


@server.list_tools()  # type: ignore[no-untyped-call,untyped-decorator]
async def _list_tools() -> list[Tool]:
    return [
        Tool(
            name="vmaf_score",
            description="Compute a VMAF score for a (reference, distorted) YUV pair. "
            "Optional tiny-AI/DNN, feature-selection, CTC-preset, and frame-range "
            "parameters map onto the corresponding vmaf CLI flags (ADR-1117).",
            inputSchema={
                "type": "object",
                "required": ["ref", "dis", "width", "height", "pixfmt", "bitdepth"],
                "properties": {
                    "ref": {"type": "string", "description": "Reference YUV path."},
                    "dis": {"type": "string", "description": "Distorted YUV path."},
                    "width": {"type": "integer", "minimum": 1},
                    "height": {"type": "integer", "minimum": 1},
                    "pixfmt": {"type": "string", "enum": ["420", "422", "444"]},
                    "bitdepth": {"type": "integer", "enum": [8, 10, 12, 16]},
                    "model": {"type": "string", "default": "version=vmaf_v0.6.1"},
                    "backend": {
                        "type": "string",
                        "enum": ["auto", "cpu", "cuda", "sycl", "hip", "metal"],
                        "default": "auto",
                    },
                    "precision": {"type": "string", "default": "legacy"},
                    **_scoring_extra_properties(),
                },
            },
        ),
        Tool(
            name="list_models",
            description="Enumerate VMAF models (JSON / pickle / ONNX) shipped with the repo.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="list_backends",
            description=(
                "Report which runtime backends (cpu / cuda / sycl / hip / metal) "
                "the local vmaf binary was built with."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="run_benchmark",
            description=(
                "Run the full multi-fixture benchmark harness (bench_all.sh) "
                "across all available backends (CPU, CUDA, SYCL) on "
                "three canonical YUV fixture sets: the 576x324 Netflix golden "
                "pair, a 1080p 5-frame pair, and the 4K BBB 200-frame pair. "
                "Returns stdout (per-backend scores + backend comparison table) "
                "and stderr. Takes no arguments — fixtures are built-in. "
                "ADR-0513."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="eval_model_on_split",
            description=(
                "Run an ONNX tiny-AI regressor on a parquet feature cache, "
                "filter to a deterministic train/val/test split (keyed by the "
                "'key' column), and report PLCC / SROCC / RMSE."
            ),
            inputSchema={
                "type": "object",
                "required": ["model", "features"],
                "properties": {
                    "model": {"type": "string", "description": "ONNX model path."},
                    "features": {"type": "string", "description": "Parquet feature cache path."},
                    "split": {"type": "string", "enum": list(_VALID_SPLITS), "default": "test"},
                    "input_name": {"type": "string", "default": "features"},
                },
            },
        ),
        Tool(
            name="compare_models",
            description=(
                "Rank several ONNX models on the same parquet feature split by "
                "descending PLCC. Models that fail to load or score are listed "
                "under 'errors' instead of aborting the whole call."
            ),
            inputSchema={
                "type": "object",
                "required": ["models", "features"],
                "properties": {
                    "models": {
                        "type": "array",
                        "items": {"type": "string"},
                        "minItems": 1,
                    },
                    "features": {"type": "string"},
                    "split": {"type": "string", "enum": list(_VALID_SPLITS), "default": "test"},
                    "input_name": {"type": "string", "default": "features"},
                },
            },
        ),
        Tool(
            name="describe_worst_frames",
            description=(
                "Score a (ref, dis) pair, pick the N worst-VMAF frames, extract "
                "each as PNG via ffmpeg, and run a vision-language model "
                "(SmolVLM → Moondream2 fallback) to describe the visible "
                "artefacts. Falls back to metadata-only output when the [vlm] "
                "extras are not installed. ADR-0172 / T6-6."
            ),
            inputSchema={
                "type": "object",
                "required": ["ref", "dis", "width", "height", "pixfmt", "bitdepth"],
                "properties": {
                    "ref": {"type": "string"},
                    "dis": {"type": "string"},
                    "width": {"type": "integer", "minimum": 1},
                    "height": {"type": "integer", "minimum": 1},
                    "pixfmt": {"type": "string", "enum": ["420", "422", "444"]},
                    "bitdepth": {"type": "integer", "enum": [8, 10, 12, 16]},
                    "model": {"type": "string", "default": "version=vmaf_v0.6.1"},
                    "backend": {
                        "type": "string",
                        "enum": ["auto", "cpu", "cuda", "sycl", "hip", "metal"],
                        "default": "auto",
                    },
                    "n": {
                        "type": "integer",
                        "minimum": 1,
                        "maximum": 32,
                        "default": 5,
                        "description": "How many worst-VMAF frames to describe.",
                    },
                },
            },
        ),
        # --- new tools (ADR-0608) ---
        Tool(
            name="probe_backend",
            description=(
                "Run a 1-frame VMAF health check to distinguish 'compiled in' from "
                "'driver present + functional'. Returns compiled_in (bool), "
                "runtime_healthy (bool), latency_ms, score (the VMAF mean on a "
                "64×64 mid-grey pair; >=36px per dimension is required by CUDA ADM), "
                "and any error string. Use this when "
                "list_backends returns true but actual GPU dispatch may fail "
                "(driver not loaded, ICD missing, KFD ioctl failure, etc.). "
                "ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["backend"],
                "properties": {
                    "backend": {
                        "type": "string",
                        "enum": ["cpu", "cuda", "sycl", "hip", "metal"],
                        "description": "Backend to health-check.",
                    },
                },
            },
        ),
        Tool(
            name="vmaf_version",
            description=(
                "Return the local vmaf binary's identity and build flags. "
                "Reports binary_path, version string (from --version), and "
                "build_flags dict (cpu/cuda/sycl/hip/metal). Use this "
                "to confirm which fork build is running before scoring. "
                "ADR-0608."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="vmaf_score_encoded",
            description=(
                "Score a (reference, distorted) pair of encoded video files "
                "(MP4, MKV, Y4M, WebM, etc.) by decoding them to raw YUV via "
                "ffmpeg and then running vmaf_score. Geometry (width, height, "
                "pixel format, bit depth) is probed automatically from the "
                "reference stream — no manual size entry required. Returns the "
                "same response shape as vmaf_score plus reference_encoded and "
                "distorted_encoded fields. Requires ffmpeg + ffprobe on PATH. "
                "ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["reference_encoded", "distorted_encoded"],
                "properties": {
                    "reference_encoded": {
                        "type": "string",
                        # ASCII "..." (not U+2026) so the schema is byte-identical
                        # to the Go server's reference_encoded description (ADR-1117).
                        "description": "Path to the reference encoded video (MP4/MKV/Y4M/...). "
                        "Must be under an allowlisted root (VMAF_MCP_ALLOW).",
                    },
                    "distorted_encoded": {
                        "type": "string",
                        "description": "Path to the distorted encoded video.",
                    },
                    "model": {"type": "string", "default": "version=vmaf_v0.6.1"},
                    "backend": {
                        "type": "string",
                        "enum": ["auto", "cpu", "cuda", "sycl", "hip", "metal"],
                        "default": "auto",
                    },
                    "subsample": {
                        "type": "integer",
                        "minimum": 1,
                        "default": 1,
                        "description": "Score every Nth frame (1 = every frame).",
                    },
                    "precision": {"type": "string", "default": "legacy"},
                    **_scoring_extra_properties(),
                },
            },
        ),
        # ── P1 tools (ADR-0608) ─────────────────────────────────────────────
        Tool(
            name="list_extractors",
            description=(
                "Enumerate all VmafFeatureExtractor implementations found in the "
                "local libvmaf C source tree. Returns each extractor's advertised "
                "name, inferred backend (cpu / cuda / sycl / hip / metal), "
                "and the source file it was defined in. Requires no binary — "
                "parses the C source directly. ADR-0608."
            ),
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="describe_model",
            description=(
                "Return metadata for a VMAF model by name or path. Accepts the "
                "model's filename stem (e.g. 'vmaf_v0.6.1'), its full filename "
                "(e.g. 'vmaf_v0.6.1.json'), or a path relative to the repo root. "
                "Fixes the Path.stem bug: 'vmaf_v0.6.1' is matched correctly "
                "against 'vmaf_v0.6.1.json' — not mis-trimmed to 'vmaf_v0.6'. "
                "Returns: name, path, format, size_bytes, model_type (JSON only), "
                "feature_names (JSON only). ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["name"],
                "properties": {
                    "name": {
                        "type": "string",
                        "description": (
                            "Model name stem (e.g. 'vmaf_v0.6.1'), full filename "
                            "(e.g. 'vmaf_v0.6.1.json'), or repo-relative path."
                        ),
                    },
                },
            },
        ),
        Tool(
            name="run_compare",
            description=(
                "Wrap 'vmaf-tune compare': compare codec adapters at one or more "
                "target VMAF scores and return a ranked report. Requires vmaf-tune "
                "to be installed (pip install -e tools/vmaf-tune). Emits MCP "
                "progress notifications when params._meta.progressToken is set. "
                "Default encoders: libx264,libx265,libsvtav1,libvpx-vp9. "
                "ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["src"],
                "properties": {
                    "src": {
                        "type": "string",
                        "description": "Source video path (any FFmpeg-readable format or raw YUV).",
                    },
                    "target_vmaf": {
                        "type": "number",
                        "description": "Single VMAF target (legacy, single-target schema).",
                    },
                    "target_vmafs": {
                        "type": "string",
                        "description": "Comma-separated VMAF targets, e.g. '94,96,97,98'.",
                    },
                    "encoders": {
                        "type": "string",
                        "description": "Comma-separated encoder list, e.g. 'libx264,libx265'.",
                    },
                    "width": {"type": "integer", "description": "Source width (raw YUV only)."},
                    "height": {"type": "integer", "description": "Source height (raw YUV only)."},
                    "pix_fmt": {"type": "string", "default": "yuv420p"},
                    "framerate": {"type": "number", "description": "Source framerate."},
                    "no_parallel": {
                        "type": "boolean",
                        "default": False,
                        "description": "Dispatch encoders sequentially (default: parallel).",
                    },
                },
            },
        ),
        Tool(
            name="run_ladder",
            description=(
                "Wrap 'vmaf-tune ladder': build a per-title bitrate ladder via "
                "convex-hull sweep over resolution x target-VMAF, pick K knees, "
                "and emit an HLS / DASH / JSON manifest. Requires vmaf-tune. "
                "Emits MCP progress notifications. ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["src", "resolutions", "target_vmafs"],
                "properties": {
                    "src": {
                        "type": "string",
                        "description": "Source video path.",
                    },
                    "resolutions": {
                        "type": "string",
                        "description": "Comma-separated WxH list, e.g. '1920x1080,1280x720,854x480'.",
                    },
                    "target_vmafs": {
                        "type": "string",
                        "description": "Comma-separated VMAF targets, e.g. '95,90,85'.",
                    },
                    "encoder": {
                        "type": "string",
                        "default": "libx264",
                        "description": "Codec adapter (default libx264).",
                    },
                    "quality_tiers": {
                        "type": "integer",
                        "default": 5,
                        "description": "Number of ladder rungs to select.",
                    },
                    "format": {
                        "type": "string",
                        "enum": ["hls", "dash", "json"],
                        "default": "json",
                        "description": "Manifest format.",
                    },
                    "spacing": {
                        "type": "string",
                        "enum": ["log_bitrate", "vmaf", "uniform"],
                        "default": "log_bitrate",
                    },
                    "framerate": {"type": "number", "description": "Source framerate."},
                },
            },
        ),
        Tool(
            name="run_tune_per_shot",
            description=(
                "Wrap 'vmaf-tune tune-per-shot': detect scene cuts, run a "
                "per-shot CRF bisect targeting a VMAF score, and return the "
                "encoding plan. Requires vmaf-tune. Emits MCP progress "
                "notifications. ADR-0608."
            ),
            inputSchema={
                "type": "object",
                "required": ["src"],
                "properties": {
                    "src": {
                        "type": "string",
                        "description": "Source video path.",
                    },
                    "target_vmaf": {
                        "type": "number",
                        "default": 92.0,
                        "description": "Target VMAF score.",
                    },
                    "encoder": {
                        "type": "string",
                        "default": "libx264",
                    },
                    "pix_fmt": {"type": "string", "default": "yuv420p"},
                    "framerate": {"type": "number"},
                    "scene_threshold": {
                        "type": "number",
                        "description": "Scene-cut detection threshold (0..1).",
                    },
                    "output": {
                        "type": "string",
                        "description": "Output video path (optional; plan-only if omitted).",
                    },
                    "format": {
                        "type": "string",
                        "enum": ["json", "shell", "csv"],
                        "default": "json",
                    },
                },
            },
        ),
    ]


# Same untyped-decorator caveat as @server.list_tools() above.
@server.call_tool()  # type: ignore[untyped-decorator]
async def _call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    # Extract MCP progress token from the request context's meta field.
    # The client opts in by passing {"_meta": {"progressToken": <token>}} in the
    # tools/call params object (MCP spec §notifications/progress).
    progress_token: str | int | None = None
    try:
        # request is typed Any | None by the mcp lib; defensively guarded by
        # the (LookupError, AttributeError) except below.
        meta = server.request_context.request.params.meta  # type: ignore[union-attr]
        if meta is not None:
            progress_token = getattr(meta, "progressToken", None)
    except (LookupError, AttributeError):
        # No active request context (e.g. unit tests) or params.meta is absent;
        # progress notifications will simply be skipped for this call.
        pass
    # ADR-0608 / E-1 (spec-correctness fix): do NOT catch exceptions here.
    # Raising from this handler lets the mcp library's outer try/except
    # (mcp/server/lowlevel/server.py _make_error_result) set isError=True on
    # the CallToolResult.  The previous pattern caught everything and returned
    # TextContent({"error": ...}) with isError implicitly False — which caused
    # conformant MCP clients (mcp/client/session.py:L394) to treat tool errors
    # as successful responses, then fail later when trying to parse the error
    # JSON as a score result.
    #
    # KeyError conversion: a missing required argument raises a raw KeyError
    # which is opaque to the caller ("KeyError: 'ref'" gives no tool context).
    # We wrap the dispatch body with a targeted try/except that converts KeyError
    # to ValueError so the mcp library surfaces it as a readable validation error
    # (isError=True, message includes the tool name and the missing key).  The
    # outer try/except does NOT swallow the error — it just translates it.
    try:
        return await _call_tool_dispatch(name, arguments, progress_token)
    except KeyError as _ke:
        raise ValueError(f"tool {name!r} missing required argument: {_ke.args[0]!r}") from _ke


async def _call_tool_dispatch(
    name: str, arguments: dict[str, Any], progress_token: str | int | None
) -> list[TextContent]:
    """Inner dispatch for :func:`_call_tool`.

    Separated so ``_call_tool`` can wrap it with a single KeyError-to-ValueError
    converter without duplicating the dispatch logic.
    """
    # Depth guard: reject pathologically nested payloads before the pydantic
    # parser recurses into them.  500-level deep dicts hit Python's recursion
    # limit inside pydantic, producing an exception that the mcp transport
    # cannot convert to a well-formed JSON-RPC error response (it silently
    # drops the response).  Raising ValueError here lets the mcp library's
    # _make_error_result wrapper return a proper isError=True tool result.
    _check_depth(arguments)

    if name == "vmaf_score":
        extras = _extras_from_args(arguments)
        # NR-mode consistency (mirrors cli_parse.c:997): --no-reference requires
        # an NR tiny model, so reject early with the same message the CLI emits.
        if extras.no_reference and not extras.tiny_model:
            raise ValueError("no_reference requires tiny_model; no classic NR scorer exists")
        # In NR mode the reference path is optional (only the distorted picture
        # is scored). A caller may still supply one; validate it when present.
        ref_arg = arguments.get("ref")
        if ref_arg:
            ref_path: Path | None = _validate_path(ref_arg)
        elif extras.no_reference:
            ref_path = None
        else:
            raise ValueError("missing required argument: 'ref' (omit only with no_reference=true)")
        req = ScoreRequest(
            ref=ref_path,
            dis=_validate_path(arguments["dis"]),
            width=int(arguments["width"]),
            height=int(arguments["height"]),
            pixfmt=str(arguments["pixfmt"]),
            bitdepth=int(arguments["bitdepth"]),
            model=str(arguments.get("model", "version=vmaf_v0.6.1")),
            backend=str(arguments.get("backend", "auto")),
            precision=str(arguments.get("precision", "legacy")),
            extras=extras,
        )
        result = await _run_vmaf_score(req)
    elif name == "list_models":
        result = {"models": _list_models()}
    elif name == "list_backends":
        result = await _list_backends()
    elif name == "run_benchmark":
        result = await _run_benchmark(progress_token=progress_token)
    elif name == "eval_model_on_split":
        result = _eval_model_on_split(
            model=_validate_path(arguments["model"]),
            features=_validate_path(arguments["features"]),
            split=str(arguments.get("split", "test")),
            input_name=str(arguments.get("input_name", "features")),
        )
    elif name == "compare_models":
        models_in = arguments["models"]
        if not isinstance(models_in, list) or not models_in:
            raise ValueError("'models' must be a non-empty list of paths")
        result = _compare_models(
            models=[_validate_path(m) for m in models_in],
            features=_validate_path(arguments["features"]),
            split=str(arguments.get("split", "test")),
            input_name=str(arguments.get("input_name", "features")),
        )
    elif name == "describe_worst_frames":
        req = ScoreRequest(
            ref=_validate_path(arguments["ref"]),
            dis=_validate_path(arguments["dis"]),
            width=int(arguments["width"]),
            height=int(arguments["height"]),
            pixfmt=str(arguments["pixfmt"]),
            bitdepth=int(arguments["bitdepth"]),
            model=str(arguments.get("model", "version=vmaf_v0.6.1")),
            backend=str(arguments.get("backend", "auto")),
        )
        n_raw = int(arguments.get("n", 5))
        if n_raw < 1 or n_raw > 32:
            raise ValueError(f"'n' must be between 1 and 32 (schema maximum); got {n_raw}")
        result = await _describe_worst_frames(req, n=n_raw)
    elif name == "probe_backend":
        # Missing 'backend' raises KeyError, converted to a uniform
        # "tool 'probe_backend' missing required argument: 'backend'"
        # ValueError by the _call_tool wrapper (matching every other
        # required-arg tool, e.g. describe_model's 'name'). The schema
        # marks 'backend' required; this keeps the error message
        # consistent instead of a bespoke "'backend' is required" string.
        result = await _probe_backend(str(arguments["backend"]))
    elif name == "vmaf_version":
        result = await _vmaf_version()
    elif name == "vmaf_score_encoded":
        encoded_extras = _extras_from_args(arguments)
        if encoded_extras.no_reference and not encoded_extras.tiny_model:
            raise ValueError("no_reference requires tiny_model; no classic NR scorer exists")
        result = await _run_vmaf_score_encoded(
            ref_path=_validate_path(arguments["reference_encoded"]),
            dis_path=_validate_path(arguments["distorted_encoded"]),
            model=str(arguments.get("model", "version=vmaf_v0.6.1")),
            backend=str(arguments.get("backend", "auto")),
            subsample=int(arguments.get("subsample", 1)),
            precision=str(arguments.get("precision", "legacy")),
            extras=encoded_extras,
        )
    # ── P1 tools (ADR-0608) ─────────────────────────────────────────────
    elif name == "list_extractors":
        result = {"extractors": _list_extractors()}
    elif name == "describe_model":
        result = _describe_model(str(arguments["name"]))
    elif name == "run_compare":
        result = await _run_compare(
            src=str(arguments["src"]),
            target_vmaf=arguments.get("target_vmaf"),
            target_vmafs=arguments.get("target_vmafs"),
            encoders=arguments.get("encoders"),
            format="json",
            width=int(arguments["width"]) if "width" in arguments else None,
            height=int(arguments["height"]) if "height" in arguments else None,
            pix_fmt=str(arguments.get("pix_fmt", "yuv420p")),
            framerate=float(arguments["framerate"]) if "framerate" in arguments else None,
            no_parallel=bool(arguments.get("no_parallel", False)),
            progress_token=progress_token,
        )
    elif name == "run_ladder":
        result = await _run_ladder(
            src=str(arguments["src"]),
            resolutions=str(arguments["resolutions"]),
            target_vmafs=str(arguments["target_vmafs"]),
            encoder=str(arguments.get("encoder", "libx264")),
            quality_tiers=int(arguments.get("quality_tiers", 5)),
            format=str(arguments.get("format", "json")),
            spacing=str(arguments.get("spacing", "log_bitrate")),
            framerate=float(arguments["framerate"]) if "framerate" in arguments else None,
            progress_token=progress_token,
        )
    elif name == "run_tune_per_shot":
        result = await _run_tune_per_shot(
            src=str(arguments["src"]),
            target_vmaf=float(arguments.get("target_vmaf", 92.0)),
            encoder=str(arguments.get("encoder", "libx264")),
            output=str(arguments["output"]) if "output" in arguments else None,
            pix_fmt=str(arguments.get("pix_fmt", "yuv420p")),
            framerate=float(arguments["framerate"]) if "framerate" in arguments else None,
            scene_threshold=(
                float(arguments["scene_threshold"]) if "scene_threshold" in arguments else None
            ),
            format=str(arguments.get("format", "json")),
            progress_token=progress_token,
        )
    else:
        raise ValueError(f"unknown tool: {name}")
    return [TextContent(type="text", text=_dumps_strict(result))]


def _check_depth(obj: Any, max_depth: int = 50, depth: int = 0) -> None:
    """Validate that a JSON-deserialised object does not exceed *max_depth* levels.

    A payload with 500+ levels of nested dicts can push the pydantic / json
    validator past Python's recursion limit, causing an unhandled exception
    that the transport layer cannot convert to a well-formed JSON-RPC error
    response.  Checking eagerly here lets us raise a plain ``ValueError``
    (which the mcp library wraps into an isError=True tool result) before we
    ever reach the recursive parser.

    Only ``dict`` and ``list`` containers contribute to nesting depth;
    scalar leaves are counted at the same level as their parent container.
    """
    if depth > max_depth:
        raise ValueError(
            f"argument nesting exceeds maximum depth ({max_depth}); "
            "reduce payload nesting and retry"
        )
    if isinstance(obj, dict):
        for v in obj.values():
            _check_depth(v, max_depth, depth + 1)
    elif isinstance(obj, list):
        for item in obj:
            _check_depth(item, max_depth, depth + 1)


def _emit_parse_error(raw_line: str, exc: Exception) -> None:
    """Write a conformant JSON-RPC parse-error response to stdout.

    Called when the server's stdin reader encounters a line that is not valid
    JSON-RPC.  JSON-RPC 2.0 §5 requires:

    .. code-block:: json

        {"jsonrpc": "2.0", "id": null, "error": {"code": -32700, "message": "Parse error"}}

    Special case: if ``raw_line`` is valid JSON but is a JSON array (a batch
    request), the mcp library does not support batching.  Per JSON-RPC 2.0 §6
    servers that do not support batch requests SHOULD respond with error code
    ``-32600`` (Invalid Request) rather than ``-32700`` (Parse error), because
    the payload is syntactically valid JSON — it is the *request* that is
    invalid, not the encoding.

    We attempt to recover the ``id`` field from the raw bytes in case the
    payload was valid JSON but not a valid JSON-RPC message (e.g. missing
    ``jsonrpc`` field) — the spec says ``id`` MUST be ``null`` when the
    message cannot be parsed, but recovering it when possible improves
    debuggability without violating conformance.

    The write is intentionally synchronous (``sys.stdout.write``) because
    the caller operates in a context where the mcp library's async stdout
    writer may not have been set up yet, and because parse errors should be
    acknowledged immediately rather than queued behind in-flight tool results.
    """
    _id: str | int | None = None
    _error_code = -32700
    _error_message = "Parse error"
    try:
        _partial = json.loads(raw_line)
        if isinstance(_partial, list):
            # Valid JSON array: this is a batch request.  The mcp library does
            # not implement JSON-RPC 2.0 batching; respond with -32600
            # (Invalid Request) because the payload is syntactically correct
            # JSON — it is the request structure that is not supported.
            _error_code = -32600
            _error_message = "Invalid Request"
        else:
            _raw_id = _partial.get("id") if isinstance(_partial, dict) else None
            if isinstance(_raw_id, (str, int)):
                _id = _raw_id
    except Exception:
        # raw_line is not valid JSON (or not a dict); _id stays None and the
        # JSON-RPC error response will carry id=null, which is spec-conformant.
        pass

    _err_payload = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": _id,
            "error": {
                "code": _error_code,
                "message": _error_message,
                "data": str(exc),
            },
        }
    )
    sys.stdout.write(_err_payload + "\n")
    sys.stdout.flush()
    _logger.debug("JSON-RPC %s on stdin (id=%r): %s", _error_message, _id, exc)


class _ParseErrorFilteredStdin:
    """Async-iterable stdin wrapper that intercepts JSON-RPC parse errors.

    The mcp library's ``stdio_server`` iterates over the supplied *stdin*
    object with ``async for line in stdin`` and passes each line to
    ``types.JSONRPCMessage.model_validate_json(line)``.  When that call
    raises, the library forwards the bare ``Exception`` onto the read stream;
    the low-level server then emits a ``notifications/message`` notification
    instead of the spec-required JSON-RPC error response (JSON-RPC 2.0 §5).

    By pre-validating each line here and writing the conformant error response
    directly to stdout before the mcp library sees the line, we ensure that
    malformed-JSON inputs are acknowledged correctly.  Lines that do not
    survive pre-validation are dropped (the mcp library never sees them),
    which prevents the notification path from running at all.

    ``sys.stdout.write`` is intentionally synchronous: it is safe from async
    context and pytest's capsys fixture captures it correctly.
    """

    def __init__(self) -> None:
        # Inner stream is created lazily in __anext__ so that instantiation
        # (which happens before the async event loop processes any messages)
        # does not create file objects that would conflict with pytest's
        # stdout/stdin capture fixtures.
        self._inner: Any = None

    def _make_inner(self) -> Any:
        """Construct the underlying async stdin reader on first use."""
        from io import TextIOWrapper

        import anyio

        # Wrap the real stdin binary buffer for async line-by-line reading.
        # Guard with hasattr in case the runtime (pytest pseudofile) does not
        # expose a binary buffer.
        if hasattr(sys.stdin, "buffer"):
            return anyio.wrap_file(
                TextIOWrapper(sys.stdin.buffer, encoding="utf-8", errors="replace")
            )
        return sys.stdin  # type: ignore[return-value]

    def __aiter__(self) -> "_ParseErrorFilteredStdin":
        return self

    async def __anext__(self) -> str:
        import mcp.types as _mcp_types

        if self._inner is None:
            self._inner = self._make_inner()

        async for raw_line in self._inner:
            try:
                _mcp_types.JSONRPCMessage.model_validate_json(raw_line)
            except Exception as exc:
                _emit_parse_error(raw_line, exc)
                continue  # drop invalid line; do not forward to mcp library
            return raw_line
        raise StopAsyncIteration


async def _run() -> None:
    if not shutil.which("meson"):
        print("warning: meson not on PATH — benchmark tool may fail.", file=sys.stderr)
    async with stdio_server(stdin=_ParseErrorFilteredStdin()) as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())


def main() -> None:
    # ADR-1023: VMAF_MCP_ASYNC truthy-string guard.
    # Accept only the well-defined anyio backend names ("asyncio", "trio")
    # or the canonical truthy tokens ("1", "true", "yes", case-insensitive)
    # as a synonym for "trio" (the most common non-asyncio anyio backend).
    # Reject ambiguous strings that would otherwise be passed verbatim to
    # anyio.run() and produce a confusing "unknown backend" RuntimeError.
    _raw_async = os.environ.get("VMAF_MCP_ASYNC", "").strip().lower()
    if _raw_async in ("", "asyncio", "0", "false", "no"):
        asyncio.run(_run())
    elif _raw_async in ("1", "true", "yes", "trio"):
        import anyio

        anyio.run(_run, backend="trio")
    else:
        # Treat the value as an explicit anyio backend name (e.g. "uvloop").
        import anyio

        anyio.run(_run, backend=_raw_async)


if __name__ == "__main__":
    main()
