# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# ai/sidecar/online_trainer.py — Python sidecar process for online model fine-tuning.
#
# This module is the entry point for the vmafx-sidecar container.  It:
#   1. Listens on a Unix domain socket for (features, true_score) messages
#      from the co-located vmafx-node Go process.
#   2. Accumulates samples in a replay buffer (ReplayBuffer, default 10 000).
#   3. Runs online SGD + EMA weight updates (SGDEMATrainer).
#   4. Periodically exports the EMA model to ONNX and writes a SHA-256
#      sidecar file — atomic rename so nodes never load a partial write.
#
# Wire protocol (Unix socket, newline-delimited JSON):
#   Client (Go) → Server (Python):
#       {"job_id": "...", "features": [f0, f1, ...], "true_score": 42.7}
#   Server → Client (ACK or error):
#       {"ok": true, "step": 1234}
#       {"ok": true, "trained": false, "retry_queued": true, "training_error": "..."}
#       {"ok": false, "error": "message"}
#       {"ok": false, "retryable": true, "error": "message"}
#
# The Unix socket path defaults to /tmp/vmafx-sidecar.sock and is
# overridden by VMAFX_SIDECAR_SOCKET.  Checkpoints default to
# /mnt/vmafx-models/online and are overridden by VMAFX_SIDECAR_CHECKPOINT_DIR.
# The current Helm chart does not wire this helper into the node pod; a
# standalone integrator must arrange a same-UID peer, a private parent
# directory, and a writable checkpoint directory before starting the server.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import contextlib
import errno
import hashlib
import json
import logging
import os
import pathlib
import signal
import socket
import stat
import sys
import threading
import time
from typing import Any, Generator

from .replay_buffer import ReplayBuffer, Sample
from .sgd_ema import SGDEMAConfig, SGDEMATrainer

_fcntl: Any
try:
    import fcntl as _fcntl
except ImportError:
    _fcntl = None

logger = logging.getLogger(__name__)


def _mark_retryable(exc: RuntimeError | ValueError) -> RuntimeError | ValueError:
    """Mark an ingest error whose triggering sample was not admitted."""
    exc._vmafx_retryable = True  # type: ignore[union-attr]
    return exc


# ---------------------------------------------------------------------------
# Configuration from environment
# ---------------------------------------------------------------------------

_SOCKET_PATH = os.environ.get("VMAFX_SIDECAR_SOCKET", "/tmp/vmafx-sidecar.sock")
_BASE_MODEL_PATH = os.environ.get("VMAFX_BASE_MODEL_PATH", "")
_CHECKPOINT_DIR = os.environ.get("VMAFX_SIDECAR_CHECKPOINT_DIR", "/mnt/vmafx-models/online")
_REPLAY_BUFFER_CAPACITY = int(os.environ.get("VMAFX_SIDECAR_REPLAY_CAPACITY", "10000"))
_PENDING_CAPACITY = int(os.environ.get("VMAFX_SIDECAR_PENDING_CAPACITY", "10000"))
_BATCH_SIZE = int(os.environ.get("VMAFX_SIDECAR_BATCH_SIZE", "32"))
_REPLAY_MIX_RATIO = float(os.environ.get("VMAFX_SIDECAR_REPLAY_MIX", "0.5"))
_LR = float(os.environ.get("VMAFX_SIDECAR_LR", "0.0001"))
_EMA_DECAY = float(os.environ.get("VMAFX_SIDECAR_EMA_DECAY", "0.999"))
_CHECKPOINT_INTERVAL_S = float(os.environ.get("VMAFX_SIDECAR_CKPT_INTERVAL_S", "600"))
_MIN_SAMPLES_PER_CKPT = int(os.environ.get("VMAFX_SIDECAR_MIN_SAMPLES_CKPT", "1000"))


# ---------------------------------------------------------------------------
# Tiny fallback model (no pre-trained base available)
# ---------------------------------------------------------------------------


def _build_fallback_model(n_features: int) -> Any:
    """Return a two-layer MLP used when no pre-trained base model is supplied.

    Architecture: Linear(n_features, 64) → ReLU → Linear(64, 1).
    This is intentionally minimal — the sidecar is designed to fine-tune
    an existing model, not train from scratch.  The fallback exists only
    so the sidecar can start accepting samples immediately without blocking
    on a base model that is temporarily unavailable.
    """
    import torch.nn as nn

    return nn.Sequential(
        nn.Linear(n_features, 64),
        nn.ReLU(),
        nn.Linear(64, 1),
    )


def _load_base_model(path: str, n_features: int) -> Any:
    """Load a pre-trained model from *path* (ONNX or PyTorch state-dict).

    Falls back to the two-layer MLP when *path* is empty or inaccessible.
    """
    if not path or not os.path.exists(path):
        logger.warning(
            "Base model not found at %r — starting from untrained fallback MLP.",
            path,
        )
        return _build_fallback_model(n_features)

    try:
        import torch

        if path.endswith(".onnx"):
            # Load ONNX via onnx2torch (optional dep) or the fallback MLP.
            try:
                import onnx2torch  # type: ignore[import-not-found]

                model = onnx2torch.convert(path)
                logger.info("Loaded base ONNX model via onnx2torch from %r", path)
                return model
            except ImportError:
                logger.warning(
                    "onnx2torch not available — cannot load ONNX base model; "
                    "falling back to untrained MLP."
                )
                return _build_fallback_model(n_features)
        else:
            # Assume PyTorch state-dict (.pt / .pth)
            model = _build_fallback_model(n_features)
            state = torch.load(path, map_location="cpu", weights_only=True)
            model.load_state_dict(state)
            logger.info("Loaded base PyTorch state-dict from %r", path)
            return model
    except Exception as exc:
        logger.error("Failed to load base model from %r: %s — using fallback MLP", path, exc)
        return _build_fallback_model(n_features)


# ---------------------------------------------------------------------------
# Checkpoint helpers
# ---------------------------------------------------------------------------


def _write_sha256_sidecar(onnx_path: str) -> None:
    """Write a <path>.sha256 file beside the ONNX checkpoint."""
    sha = hashlib.sha256()
    with open(onnx_path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            sha.update(chunk)
    digest = sha.hexdigest()
    sidecar_path = onnx_path + ".sha256"
    tmp_path = sidecar_path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as fh:
        fh.write(digest + "\n")
    os.replace(tmp_path, sidecar_path)
    logger.debug("SHA-256 sidecar written: %s = %s", sidecar_path, digest)


# ---------------------------------------------------------------------------
# OnlineTrainer — orchestrates buffer + trainer + checkpoint loop
# ---------------------------------------------------------------------------


class OnlineTrainer:
    """Orchestrates the replay buffer, SGD+EMA trainer, and checkpoint loop.

    Parameters
    ----------
    n_features:
        Dimensionality of the feature vector sent by vmafx-node.
    base_model_path:
        Path to the ONNX or PyTorch state-dict to fine-tune.
    checkpoint_dir:
        Directory where versioned ONNX checkpoints are written.
    buffer_capacity:
        Replay buffer capacity (default 10 000 per ADR-0781).
    pending_capacity:
        Maximum admitted samples not yet committed by a successful step. Must
        fit the new-sample portion of one batch.
    batch_size:
        Mini-batch size for each gradient step.
    replay_mix_ratio:
        Fraction of each batch drawn from the replay buffer (``[0.0, 1.0)``).
    config:
        SGDEMAConfig instance; defaults are used when None.
    """

    def __init__(
        self,
        n_features: int,
        base_model_path: str = "",
        checkpoint_dir: str = _CHECKPOINT_DIR,
        buffer_capacity: int = _REPLAY_BUFFER_CAPACITY,
        pending_capacity: int = _PENDING_CAPACITY,
        batch_size: int = _BATCH_SIZE,
        replay_mix_ratio: float = _REPLAY_MIX_RATIO,
        config: SGDEMAConfig | None = None,
    ) -> None:
        if batch_size <= 0:
            raise ValueError("batch_size must be positive")
        if not 0.0 <= replay_mix_ratio < 1.0:
            raise ValueError("replay_mix_ratio must be in [0.0, 1.0)")
        self._n_features = n_features
        self._checkpoint_dir = pathlib.Path(checkpoint_dir)
        self._checkpoint_dir.mkdir(parents=True, exist_ok=True)
        self._replay_samples_per_batch = int(batch_size * replay_mix_ratio)
        self._new_samples_per_batch = batch_size - self._replay_samples_per_batch
        if pending_capacity < self._new_samples_per_batch:
            raise ValueError(
                "pending_capacity must fit one batch's new-sample window "
                f"({self._new_samples_per_batch})"
            )
        self._pending_capacity = pending_capacity

        self._buffer = ReplayBuffer(capacity=buffer_capacity)
        model = _load_base_model(base_model_path, n_features)
        self._trainer = SGDEMATrainer(model, config)

        self._pending: list[Sample] = []  # samples received but not yet trained on
        self._lock = threading.Lock()
        self._training_active = False
        self._inflight_new_samples = 0
        # Separate lock guards the should_checkpoint() gate + counter increment +
        # filename choice + export so concurrent connection threads cannot race
        # the version number or export two checkpoints under the same name.
        self._checkpoint_lock = threading.Lock()
        self._checkpoint_counter: int = 0

    def ingest(self, features: list[float], true_score: float) -> dict[str, Any]:
        """Accept one (features, true_score) pair from the socket handler.

        Once enough new samples exist to fill the non-replay portion of a
        batch, reserve the oldest such window and trigger one serialized
        gradient step. Concurrent callers may enqueue behind the active window
        up to ``_pending_capacity``; calls beyond that bound receive explicit
        backpressure and must retry their sample. Returns an ACK status.

        A trainer failure restores its FIFO window. If this call's sample was
        already admitted, the result acknowledges it with ``retry_queued`` so
        the caller does not submit a duplicate. If capacity deferred admission,
        the failure propagates and the caller must retry the unaccepted sample.

        Raises ``ValueError`` for a wrong-length feature vector or a
        non-numeric ``true_score``; the socket handler converts that into a
        structured ``{"ok": false, "error": ...}`` ACK.  Validating *before*
        the buffer push keeps malformed samples out of the shared replay
        buffer, which would otherwise poison every future replay-mixed batch.
        """
        if len(features) != self._n_features:
            raise ValueError(
                f"feature vector length {len(features)} != expected {self._n_features}"
            )
        # Coerce/validate the score outside the lock so a bad value never
        # reaches the shared buffer (Sample(...) would raise mid-push otherwise).
        score = float(true_score)
        sample = Sample(tuple(float(f) for f in features), score)

        prepared = self._prepare_ingest(sample)
        if prepared is None:
            return {"ok": True, "step": self._trainer.step_count, "trained": False}
        reserved, batch, sample_deferred = prepared

        # Step outside the queue lock. _training_active serializes the complete
        # reserve -> train -> restore/commit lifecycle while concurrent calls
        # enqueue behind the reserved FIFO window.
        try:
            loss = self._train_on_batch(batch, len(reserved))
        except (RuntimeError, ValueError) as exc:
            # The gradient step failed (CUDA OOM, prediction/target mismatch,
            # ...). Restore its FIFO window before releasing the training owner.
            self._finish_training(restore=reserved)
            if sample_deferred:
                _mark_retryable(exc)
                raise
            return self._accepted_failure_result(exc)

        # Check checkpoint condition and export atomically under the checkpoint
        # lock — otherwise two connection threads can both pass should_checkpoint()
        # and collide on the same model_vNNNNNN.onnx version (R3-7).
        ckpt_path = self._commit_training_step(sample if sample_deferred else None)

        return {
            "ok": True,
            "step": self._trainer.step_count,
            "trained": True,
            "loss": loss,
            "checkpoint": ckpt_path,
        }

    def status(self) -> dict[str, Any]:
        """Return a status snapshot to embedding callers and tests."""
        buf_stats = self._buffer.stats
        with self._lock:
            pending_size = len(self._pending) + self._inflight_new_samples
            training_active = self._training_active
        return {
            "step_count": self._trainer.step_count,
            "buffer_size": buf_stats["current_size"],
            "buffer_capacity": buf_stats["capacity"],
            "total_pushed": buf_stats["total_pushed"],
            "total_evicted": buf_stats["total_evicted"],
            "checkpoint_counter": self._checkpoint_counter,
            "n_features": self._n_features,
            "pending_size": pending_size,
            "pending_capacity": self._pending_capacity,
            "training_active": training_active,
        }

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _build_batch(self, reserved: list[Sample]) -> list[Sample]:
        """Combine a reserved pending window with a replay draw."""
        replay_samples = self._buffer.sample(self._replay_samples_per_batch)
        return reserved + replay_samples

    def _accepted_failure_result(self, exc: Exception) -> dict[str, Any]:
        """Acknowledge an admitted sample whose training window remains queued."""
        logger.warning("Training step deferred; admitted samples remain queued: %s", exc)
        return {
            "ok": True,
            "step": self._trainer.step_count,
            "trained": False,
            "retry_queued": True,
            "training_error": str(exc),
        }

    def _prepare_ingest(self, sample: Sample) -> tuple[list[Sample], list[Sample], bool] | None:
        """Admit a sample or use its call to drain a capacity-bound backlog."""
        with self._lock:
            sample_deferred = self._backlog_size_locked() >= self._pending_capacity
            if sample_deferred:
                if self._training_active:
                    raise _mark_retryable(
                        RuntimeError(
                            f"pending training queue is full ({self._pending_capacity} samples); "
                            "retry later"
                        )
                    )
            else:
                self._pending.append(sample)
                self._buffer.push(sample.features, sample.true_score)

            if self._training_active or len(self._pending) < self._new_samples_per_batch:
                return None

            reserved = self._pending[: self._new_samples_per_batch]
            batch = self._build_batch(reserved)
            del self._pending[: self._new_samples_per_batch]
            self._training_active = True
            self._inflight_new_samples = len(reserved)
            return reserved, batch, sample_deferred

    def _backlog_size_locked(self) -> int:
        """Return admitted, uncommitted samples while ``_lock`` is held."""
        return len(self._pending) + self._inflight_new_samples

    def _finish_training(
        self,
        restore: list[Sample] | None = None,
        admit: Sample | None = None,
    ) -> None:
        """Release the training owner, restoring or admitting as requested."""
        with self._lock:
            if restore is not None:
                self._pending[:0] = restore
            self._inflight_new_samples = 0
            if admit is not None:
                self._pending.append(admit)
                self._buffer.push(admit.features, admit.true_score)
            self._training_active = False

    def _commit_training_step(self, admit: Sample | None = None) -> str | None:
        """Export if due, then release the single training owner."""
        try:
            return self._maybe_export_checkpoint()
        finally:
            self._finish_training(admit=admit)

    def _train_on_batch(self, batch: list[Sample], new_sample_count: int) -> float:
        """Convert batch to tensors and call trainer.step()."""
        import torch

        if not batch:
            return 0.0
        feats = torch.tensor([s.features for s in batch], dtype=torch.float32)
        scores = torch.tensor([s.true_score for s in batch], dtype=torch.float32)
        return self._trainer.step(feats, scores, new_sample_count=new_sample_count)

    def _maybe_export_checkpoint(self) -> str | None:
        """Atomically gate-check and export a checkpoint.

        Holds ``_checkpoint_lock`` across ``should_checkpoint()``, the counter
        increment, the filename choice, and the export.  Without this, two
        concurrent connection threads could both observe ``should_checkpoint()``
        as True (it only resets *after* a successful export) and produce two
        checkpoints sharing one ``model_vNNNNNN.onnx`` name, one silently
        overwriting the other (R3-7).
        """
        with self._checkpoint_lock:
            if not self._trainer.should_checkpoint():
                return None
            return self._export_checkpoint()

    def _export_checkpoint(self) -> str | None:
        """Export EMA model to a versioned ONNX file.

        Returns the path on success, or ``None`` if the export failed (so the
        caller's ACK carries ``"checkpoint": null`` rather than a path to a file
        that was never written).

        Callers must hold ``_checkpoint_lock`` so the counter increment and the
        version-stamped filename are atomic across threads.
        """
        # Compute the *candidate* version without mutating shared state: the
        # counter is only advanced after a successful, durable export, so a
        # failed export neither burns a version number nor returns a ghost path
        # (R3-7 follow-up).
        next_counter = self._checkpoint_counter + 1
        ckpt_name = f"model_v{next_counter:06d}.onnx"
        ckpt_path = str(self._checkpoint_dir / ckpt_name)
        try:
            self._trainer.export_onnx(ckpt_path, n_features=self._n_features)
            _write_sha256_sidecar(ckpt_path)
        except Exception as exc:
            logger.error("Checkpoint export failed: %s", exc)
            return None
        self._checkpoint_counter = next_counter
        logger.info(
            "Checkpoint exported: %s (step=%d)",
            ckpt_path,
            self._trainer.step_count,
        )
        return ckpt_path


# ---------------------------------------------------------------------------
# Unix-socket server
# ---------------------------------------------------------------------------


@contextlib.contextmanager
def _socket_path_claim(socket_path: str) -> Generator[None, None, None]:
    """Hold a non-blocking process claim across pathname inspection and serving."""
    if _fcntl is None:
        yield
        return

    lock_path = f"{socket_path}.lock"
    flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        lock_fd = os.open(lock_path, flags, 0o600)
    except OSError as exc:
        if exc.errno == errno.ELOOP:
            raise FileExistsError(
                errno.EEXIST,
                f"refusing symlink socket claim path: {lock_path}",
                lock_path,
            ) from exc
        raise

    locked = False
    try:
        fd_stat = os.fstat(lock_fd)
        path_stat = os.lstat(lock_path)
        if not stat.S_ISREG(path_stat.st_mode) or (path_stat.st_dev, path_stat.st_ino) != (
            fd_stat.st_dev,
            fd_stat.st_ino,
        ):
            raise FileExistsError(
                errno.EEXIST,
                f"refusing non-regular socket claim path: {lock_path}",
                lock_path,
            )
        os.fchmod(lock_fd, 0o600)
        try:
            _fcntl.flock(lock_fd, _fcntl.LOCK_EX | _fcntl.LOCK_NB)
            locked = True
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EAGAIN):
                raise
            raise OSError(
                errno.EADDRINUSE,
                f"socket endpoint is already claimed: {socket_path}",
                socket_path,
            ) from exc
        yield
    finally:
        if locked:
            with contextlib.suppress(OSError):
                _fcntl.flock(lock_fd, _fcntl.LOCK_UN)
        os.close(lock_fd)


def _prepare_socket_path(socket_path: str) -> None:
    """Remove one unchanged, non-listening Unix socket left by a prior process."""
    try:
        candidate = os.lstat(socket_path)
    except FileNotFoundError:
        return

    if not stat.S_ISSOCK(candidate.st_mode):
        raise FileExistsError(
            errno.EEXIST,
            f"refusing to replace non-socket path: {socket_path}",
            socket_path,
        )

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
        # A blocking AF_UNIX connect can wait indefinitely when a live listener's
        # accept queue is full.  Probe once in non-blocking mode: only an explicit
        # ECONNREFUSED proves a stale candidate; every pending or resource-pressure
        # result is an active/unverified endpoint and must fail closed.
        probe.setblocking(False)
        try:
            probe_result = probe.connect_ex(socket_path)
        except FileNotFoundError:
            return
        except OSError as exc:
            raise OSError(
                errno.EADDRINUSE,
                f"refusing to replace unverified socket endpoint: {socket_path}",
            ) from exc
        if probe_result == 0:
            raise OSError(errno.EADDRINUSE, f"socket endpoint is already active: {socket_path}")
        if probe_result == errno.ENOENT:
            return
        if probe_result != errno.ECONNREFUSED:
            raise OSError(
                errno.EADDRINUSE,
                "refusing to replace active or unverified socket endpoint "
                f"({os.strerror(probe_result)}): {socket_path}",
            )

    try:
        current = os.lstat(socket_path)
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(current.st_mode) or (current.st_dev, current.st_ino) != (
        candidate.st_dev,
        candidate.st_ino,
    ):
        raise OSError(errno.EADDRINUSE, f"socket endpoint changed during probe: {socket_path}")
    os.unlink(socket_path)


def _socket_path_identity(socket_path: str) -> tuple[int, int] | None:
    """Return the device/inode pair for a pathname socket without following links."""
    try:
        path_stat = os.lstat(socket_path)
    except FileNotFoundError:
        return None
    if not stat.S_ISSOCK(path_stat.st_mode):
        return None
    return path_stat.st_dev, path_stat.st_ino


def _unlink_socket_if_owned(socket_path: str, owned_identity: tuple[int, int] | None) -> None:
    """Best-effort cleanup that never removes a path with a different identity."""
    if owned_identity is None or _socket_path_identity(socket_path) != owned_identity:
        return
    with contextlib.suppress(FileNotFoundError):
        os.unlink(socket_path)


class _ConnectionRegistry:
    """Thread-safe registry for active client connections.

    Encapsulates connection tracking and synchronization into an atomic
    abstraction, preventing invalid half-state from separate collection and lock
    parameters while preserving deterministic shutdown semantics.
    """

    def __init__(self) -> None:
        self._conns: set[socket.socket] = set()
        self._lock = threading.Lock()

    def register_if_active(self, conn: socket.socket, stop_event: threading.Event) -> bool:
        """Atomically register *conn* if *stop_event* is clear.

        If *stop_event* is set, closes *conn* immediately and returns False,
        preventing connection leaks during shutdown races.
        """
        with self._lock:
            if stop_event.is_set():
                with contextlib.suppress(OSError):
                    conn.close()
                return False
            self._conns.add(conn)
            return True

    def discard(self, conn: socket.socket) -> None:
        """Atomically remove *conn* without closing it."""
        with self._lock:
            self._conns.discard(conn)

    def close_all(self) -> None:
        """Atomically extract all connections, shut them down, and close them."""
        with self._lock:
            conns = list(self._conns)
            self._conns.clear()
        for c in conns:
            with contextlib.suppress(OSError):
                c.shutdown(socket.SHUT_RDWR)
            with contextlib.suppress(OSError):
                c.close()

    def __len__(self) -> int:
        with self._lock:
            return len(self._conns)


def _handle_connection(
    conn: socket.socket,
    trainer: OnlineTrainer,
    registry: _ConnectionRegistry | None = None,
) -> None:
    """Handle one client connection (vmafx-node goroutine).

    Reads newline-delimited JSON messages, calls trainer.ingest(), and
    writes JSON ACK responses.  The connection is closed when the client
    disconnects or sends malformed data.
    """
    try:
        addr = conn.getpeername() if conn.family != socket.AF_UNIX else "<unix>"
    except OSError:
        addr = "<closed>"
    logger.debug("Connection from %s", addr)
    buf = b""
    try:
        while chunk := conn.recv(65536):
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                    features = msg["features"]
                    true_score = msg["true_score"]
                    result = trainer.ingest(features, true_score)
                    result["job_id"] = msg.get("job_id", "")
                    ack = json.dumps(result) + "\n"
                    conn.sendall(ack.encode())
                except (KeyError, json.JSONDecodeError, TypeError, ValueError, RuntimeError) as exc:
                    # Already-admitted trainer failures return normally with
                    # retry_queued. Exceptions reaching here are malformed input or
                    # failures for a sample that was not admitted, so ok:false is
                    # retry-safe and must not kill the connection thread (R3-3).
                    error_result = {"ok": False, "error": str(exc)}
                    if getattr(exc, "_vmafx_retryable", False):
                        error_result["retryable"] = True
                    err = json.dumps(error_result) + "\n"
                    conn.sendall(err.encode())
    except OSError as exc:
        logger.debug("Connection closed: %s", exc)
    finally:
        if registry is not None:
            registry.discard(conn)
        with contextlib.suppress(OSError):
            conn.close()
    logger.debug("Connection from %s closed", addr)


def _run_server_with_claim(
    socket_path: str = _SOCKET_PATH,
    trainer: OnlineTrainer | None = None,
    n_features: int = 80,
    stop_event: threading.Event | None = None,
) -> None:
    """Start the Unix-socket server.  Blocks until SIGTERM/SIGINT.

    Parameters
    ----------
    socket_path:
        Path for the Unix domain socket. The file is unlinked on shutdown only
        while it still has the device/inode identity created by this server.
    trainer:
        ``OnlineTrainer`` instance.  Constructed with defaults when None.
    n_features:
        Feature vector dimension.  Used only when *trainer* is None.
    stop_event:
        Optional ``threading.Event`` to trigger shutdown programmatically.
    """
    if trainer is None:
        trainer = OnlineTrainer(
            n_features=n_features,
            base_model_path=_BASE_MODEL_PATH,
            config=SGDEMAConfig(
                lr=_LR,
                ema_decay=_EMA_DECAY,
                checkpoint_interval_s=_CHECKPOINT_INTERVAL_S,
                min_samples_per_checkpoint=_MIN_SAMPLES_PER_CKPT,
            ),
        )

    _prepare_socket_path(socket_path)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    threads: list[threading.Thread] = []
    registry = _ConnectionRegistry()
    old_sigterm: Any = None
    old_sigint: Any = None
    owned_socket_identity: tuple[int, int] | None = None

    try:
        srv.bind(socket_path)
        owned_socket_identity = _socket_path_identity(socket_path)
        if owned_socket_identity is None:
            raise OSError(
                errno.EADDRINUSE,
                f"bound socket path was replaced before publication: {socket_path}",
            )
        # The shipped deployment runs both peers under one UID. Keep the
        # unauthenticated local endpoint owner-only by default.
        os.chmod(socket_path, 0o600, follow_symlinks=False)
        if _socket_path_identity(socket_path) != owned_socket_identity:
            raise OSError(
                errno.EADDRINUSE,
                f"bound socket path changed during publication: {socket_path}",
            )
        srv.listen(16)
        srv.settimeout(1.0)  # allows the signal check below to fire promptly

        _stop = stop_event if stop_event is not None else threading.Event()

        def _sighandler(signum: int, _frame: Any) -> None:
            logger.info("Received signal %d — initiating shutdown", signum)
            _stop.set()

        with contextlib.suppress(ValueError):
            old_sigterm = signal.signal(signal.SIGTERM, _sighandler)
            old_sigint = signal.signal(signal.SIGINT, _sighandler)

        logger.info("vmafx-sidecar listening on %s", socket_path)

        while not _stop.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                if _stop.is_set():
                    break
                raise

            if not registry.register_if_active(conn, _stop):
                break

            try:
                t = threading.Thread(
                    target=_handle_connection,
                    args=(conn, trainer, registry),
                    daemon=True,
                )
                t.start()
            except Exception:
                registry.discard(conn)
                with contextlib.suppress(OSError):
                    conn.close()
                raise
            threads.append(t)
            # Prune dead threads to avoid unbounded list growth.
            threads = [tt for tt in threads if tt.is_alive()]
    finally:
        with contextlib.suppress(ValueError):
            if old_sigterm is not None:
                signal.signal(signal.SIGTERM, old_sigterm)
            if old_sigint is not None:
                signal.signal(signal.SIGINT, old_sigint)

        # Promptly unblock and close any held client connections
        registry.close_all()

        deadline = time.monotonic() + 1.0
        for t in threads:
            remaining = max(0.0, deadline - time.monotonic())
            t.join(timeout=remaining)
        srv.close()
        _unlink_socket_if_owned(socket_path, owned_socket_identity)
        logger.info("vmafx-sidecar stopped")


def run_server(
    socket_path: str = _SOCKET_PATH,
    trainer: OnlineTrainer | None = None,
    n_features: int = 80,
    stop_event: threading.Event | None = None,
) -> None:
    """Start one safely claimed Unix-socket server until signal or stop event."""
    with _socket_path_claim(socket_path):
        _run_server_with_claim(socket_path, trainer, n_features, stop_event)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stderr,
    )
    n_features = int(os.environ.get("VMAFX_SIDECAR_N_FEATURES", "80"))
    run_server(n_features=n_features)


if __name__ == "__main__":
    main()
