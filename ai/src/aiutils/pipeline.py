# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Resumable stage runner for the retrain pipeline (ADR-1898, issue #1246).

One :class:`Stage` is one command plus the files it reads and the files it
writes. :func:`run_pipeline` runs a list of stages in order and gives the three
properties the one-shot retrain needs:

* **Fail named and early.** Every input that no earlier stage produces is
  checked before the first stage starts; a missing, empty or unreadable one
  raises :class:`StageError` naming the stage and the path. A stage that exits
  non-zero, leaves a declared output missing or empty, or fails its ``check``
  raises the same error with the stage name.
* **Resume.** Each stage writes ``<run>/stages/<name>.stage.json``. ``running``
  is written before the command starts and replaced atomically by ``complete``
  after the outputs are verified and hashed. A later run skips a stage only
  when its manifest is ``complete``, its *key* (argv, input digests, seed,
  environment identity) is unchanged and every output still has the recorded
  digest; anything else, including a manifest left at ``running`` by a killed
  run, runs the stage again from a clean slate of its outputs.
* **Reproducibility.** The manifest records the seed, the interpreter and
  library versions, the digest of the dependency lock, the container image id
  and the git revision, so two runs are comparable; a stage's ``stable``
  outputs are byte-identical across runs with the same inputs, seed and environment.

Resource use per stage (wall seconds, CPU seconds, peak resident set) is in
the manifest too: it is the measurement the resource plan scales from.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from aiutils.file_utils import sha256, write_text_atomic
from aiutils.time_utils import now_iso_8601

MANIFEST_SCHEMA = "retrain-stage-manifest-v1"
PIPELINE_SCHEMA = "retrain-pipeline-manifest-v1"
_LOG_TAIL_LINES = 20
_POLL_S = 0.05
_LIBS = ("torch", "onnx", "onnxruntime", "numpy", "pandas", "pyarrow", "scipy", "sklearn")


class StageError(RuntimeError):
    """A stage failed; ``stage`` names it and the message says why."""

    def __init__(self, stage: str, message: str) -> None:
        super().__init__(f"stage '{stage}': {message}")
        self.stage = stage
        self.reason = message


@dataclass(frozen=True)
class StageContext:
    """What a stage ``check`` may read."""

    run_dir: Path
    stage: str


@dataclass(frozen=True)
class Stage:
    """One command, its inputs and its outputs."""

    name: str
    argv: tuple[str, ...]
    inputs: tuple[Path, ...] = ()
    outputs: tuple[Path, ...] = ()
    seed: int | None = None
    # Outputs that are byte-identical for the same inputs, seed and environment.
    # A sidecar that embeds the run directory in its provenance block is not one.
    stable: tuple[Path, ...] = ()
    # Files hashed into the resume key beyond ``inputs`` (the stage's script).
    code: tuple[Path, ...] = ()
    check: Callable[[StageContext], None] | None = None
    # Work done, for the resource plan: {"rows": 144, ...}.
    work: Callable[[StageContext], Mapping[str, int]] | None = None
    # (source, destination) copies made after the outputs are cleared and before the
    # command runs, for a tool that updates a file in place: the destination is
    # then an output and the source an input.
    copies: tuple[tuple[Path, Path], ...] = ()
    timeout_s: float = 1800.0
    env: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class StageResult:
    """The outcome of one stage in one run."""

    name: str
    status: str  # "complete" | "resumed"
    manifest: Path


def _digest_json(payload: Any) -> str:
    text = json.dumps(payload, sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(text.encode()).hexdigest()


def _lib_versions() -> dict[str, str]:
    from importlib import metadata

    out: dict[str, str] = {}
    for lib in _LIBS:
        dist = "scikit-learn" if lib == "sklearn" else lib
        try:
            out[lib] = metadata.version(dist)
        except metadata.PackageNotFoundError:
            out[lib] = "absent"
    return out


def _git_revision(repo_root: Path) -> str:
    try:
        done = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return done.stdout.strip() if done.returncode == 0 and done.stdout.strip() else "unknown"


def _container_identity() -> str:
    """The container image id when one is given, else the host kernel line.

    A container sets ``VMAFX_CONTAINER_IMAGE_ID`` (the dev image's entrypoint
    and the CI job both do); a bare host reports itself as such so the manifest
    never claims an image it did not run in.
    """
    image = os.environ.get("VMAFX_CONTAINER_IMAGE_ID", "").strip()
    if image:
        return image
    return f"host:{platform.system()}-{platform.release()}-{platform.machine()}"


def environment_identity(repo_root: Path, lock_files: Sequence[Path]) -> dict[str, Any]:
    """Everything outside a stage's argv that can change what it writes."""
    return {
        "python": platform.python_version(),
        "libraries": _lib_versions(),
        "container": _container_identity(),
        "git_revision": _git_revision(repo_root),
        "locks": {
            str(p.relative_to(repo_root)) if p.is_relative_to(repo_root) else str(p): (
                sha256(p) if p.is_file() else "missing"
            )
            for p in lock_files
        },
    }


def _rel(path: Path, run_dir: Path) -> str:
    return str(path.relative_to(run_dir)) if path.is_relative_to(run_dir) else str(path)


def _manifest_path(run_dir: Path, name: str) -> Path:
    return run_dir / "stages" / f"{name}.stage.json"


def _read_manifest(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def _write_manifest(path: Path, payload: Mapping[str, Any]) -> None:
    write_text_atomic(path, json.dumps(payload, indent=2, sort_keys=True) + "\n")


def path_digest(path: Path) -> str:
    """SHA-256 of a file, or of a directory's relative names and file digests."""
    if path.is_file():
        return sha256(path)
    h = hashlib.sha256()
    for item in sorted(p for p in path.rglob("*") if p.is_file()):
        h.update(str(item.relative_to(path)).encode())
        h.update(b"\0")
        h.update(sha256(item).encode())
    return h.hexdigest()


def _require_file(stage: str, path: Path, role: str) -> None:
    """A declared file (or a directory holding at least one file) must exist and read."""
    if path.is_dir():
        if not any(p.is_file() for p in path.rglob("*")):
            raise StageError(stage, f"{role} directory holds no file: {path}")
        return
    if not path.is_file():
        raise StageError(stage, f"{role} missing: {path}")
    if path.stat().st_size == 0:
        raise StageError(stage, f"{role} is empty: {path}")
    try:
        with path.open("rb") as fh:
            fh.read(1)
    except OSError as exc:
        raise StageError(stage, f"{role} unreadable: {path} ({exc})") from exc


def preflight(stages: Sequence[Stage]) -> None:
    """Check every input no earlier stage produces, before anything runs."""
    produced: set[Path] = set()
    for stage in stages:
        for path in stage.inputs:
            if path not in produced:
                _require_file(stage.name, path, "input")
        produced.update(stage.outputs)


def _stage_key(stage: Stage, env_identity: Mapping[str, Any], seed_of: str) -> str:
    return _digest_json(
        {
            "argv": list(stage.argv),
            "inputs": {str(p): path_digest(p) for p in stage.inputs if p.exists()},
            "code": {str(p): path_digest(p) for p in stage.code if p.exists()},
            "seed": seed_of,
            "env": env_identity,
        }
    )


def _outputs_intact(stage: Stage, recorded: Mapping[str, str]) -> bool:
    if set(recorded) != {str(p) for p in stage.outputs}:
        return False
    return all(p.exists() and path_digest(p) == recorded[str(p)] for p in stage.outputs)


def _can_resume(stage: Stage, manifest: Mapping[str, Any] | None, key: str) -> bool:
    if manifest is None or manifest.get("status") != "complete":
        return False
    if manifest.get("key") != key:
        return False
    return _outputs_intact(stage, manifest.get("outputs", {}))


def _log_tail(path: Path) -> str:
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-_LOG_TAIL_LINES:])


@dataclass(frozen=True)
class _Resources:
    wall_s: float
    cpu_s: float | None
    peak_rss_kb: int | None


def _spawn(stage: Stage, log: Path) -> tuple[int, _Resources]:
    """Run the stage command; return its exit code and resource use."""
    env = {**os.environ, **stage.env}
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log.open("wb") as sink:
        proc = subprocess.Popen(list(stage.argv), stdout=sink, stderr=subprocess.STDOUT, env=env)
        code, cpu_s, rss = _wait(proc, stage.timeout_s)
    return code, _Resources(round(time.monotonic() - start, 3), cpu_s, rss)


def _wait(proc: subprocess.Popen[bytes], timeout_s: float) -> tuple[int, float | None, int | None]:
    """Wait for ``proc`` with a bound; POSIX also reports CPU time and peak RSS."""
    if not hasattr(os, "wait4"):
        try:
            return proc.wait(timeout=timeout_s), None, None
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return 124, None, None
    deadline = time.monotonic() + timeout_s
    # Bounded poll: the deadline is checked every pass, and the pass count is capped
    # at the timeout in 50 ms steps plus one so no path loops without limit.
    for _ in range(int(timeout_s / _POLL_S) + 2):
        pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
        if pid == proc.pid:
            proc.returncode = os.waitstatus_to_exitcode(status)
            return proc.returncode, usage.ru_utime + usage.ru_stime, int(usage.ru_maxrss)
        if time.monotonic() > deadline:
            proc.kill()
            _, status, usage = os.wait4(proc.pid, 0)
            proc.returncode = 124
            return 124, usage.ru_utime + usage.ru_stime, int(usage.ru_maxrss)
        time.sleep(_POLL_S)
    proc.kill()
    os.wait4(proc.pid, 0)
    proc.returncode = 124
    return 124, None, None


def _clear_outputs(stage: Stage) -> None:
    for path in stage.outputs:
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink(missing_ok=True)
        path.parent.mkdir(parents=True, exist_ok=True)


def _verify_outputs(stage: Stage, ctx: StageContext) -> dict[str, str]:
    for path in stage.outputs:
        _require_file(stage.name, path, "output")
    if stage.check is not None:
        stage.check(ctx)
    return {str(p): path_digest(p) for p in stage.outputs}


def _run_stage(
    stage: Stage, run_dir: Path, env_identity: Mapping[str, Any], key: str, manifest: Path
) -> None:
    """Run one stage from a clean slate and record its manifest."""
    head: dict[str, Any] = {
        "schema": MANIFEST_SCHEMA,
        "stage": stage.name,
        "argv": list(stage.argv),
        "seed": stage.seed,
        "key": key,
        "environment": env_identity,
        "inputs": {str(p): path_digest(p) for p in stage.inputs},
        "started": now_iso_8601(),
    }
    _write_manifest(manifest, {**head, "status": "running"})
    _clear_outputs(stage)
    for src, dst in stage.copies:
        shutil.copyfile(src, dst)
    log = run_dir / "logs" / f"{stage.name}.log"
    code, res = _spawn(stage, log)
    if code != 0:
        _write_manifest(manifest, {**head, "status": "failed", "returncode": code})
        raise StageError(stage.name, f"exited {code} (log {log}); last lines:\n{_log_tail(log)}")
    ctx = StageContext(run_dir=run_dir, stage=stage.name)
    try:
        outputs = _verify_outputs(stage, ctx)
    except StageError:
        _write_manifest(manifest, {**head, "status": "failed", "returncode": 0})
        raise
    work = dict(stage.work(ctx)) if stage.work is not None else {}
    _write_manifest(
        manifest,
        {
            **head,
            "status": "complete",
            "returncode": 0,
            "outputs": outputs,
            "stable": {_rel(p, run_dir): outputs[str(p)] for p in stage.stable},
            "work": work,
            "resources": {
                "wall_s": res.wall_s,
                "cpu_s": res.cpu_s,
                "peak_rss_kb": res.peak_rss_kb,
            },
            "finished": now_iso_8601(),
        },
    )


def run_pipeline(
    stages: Sequence[Stage],
    run_dir: Path,
    *,
    repo_root: Path,
    lock_files: Sequence[Path] = (),
    after_stage: Callable[[str], None] | None = None,
    log: Callable[[str], None] = print,
) -> list[StageResult]:
    """Run ``stages`` in order, resuming finished work; raise on the first failure.

    ``after_stage`` is called with a stage's name once it is complete or
    resumed; the planted-defect tests use it to corrupt an output and prove the
    next stage refuses it.
    """
    names = [s.name for s in stages]
    if len(set(names)) != len(names):
        raise ValueError(f"duplicate stage names: {names}")
    preflight(stages)
    run_dir.mkdir(parents=True, exist_ok=True)
    env_identity = environment_identity(repo_root, lock_files)
    results: list[StageResult] = []
    for stage in stages:
        manifest = _manifest_path(run_dir, stage.name)
        # Re-check inputs a previous stage was meant to produce: a stage that
        # "succeeded" without writing them is that stage's failure, and one
        # whose output a hook or a disk fault corrupted must not feed the next.
        for path in stage.inputs:
            _require_file(stage.name, path, "input")
        key = _stage_key(stage, env_identity, str(stage.seed))
        if _can_resume(stage, _read_manifest(manifest), key):
            log(f"[pipeline] {stage.name}: resumed (manifest complete, outputs unchanged)")
            results.append(StageResult(stage.name, "resumed", manifest))
        else:
            log(f"[pipeline] {stage.name}: running")
            _run_stage(stage, run_dir, env_identity, key, manifest)
            results.append(StageResult(stage.name, "complete", manifest))
        if after_stage is not None:
            after_stage(stage.name)
    _write_manifest(
        run_dir / "pipeline.manifest.json",
        {
            "schema": PIPELINE_SCHEMA,
            "stages": [{"name": r.name, "status": r.status} for r in results],
            "environment": env_identity,
            "python_executable": sys.executable,
        },
    )
    return results
