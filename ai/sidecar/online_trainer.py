# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
#       {"ok": false, "error": "message"}
#
# The Unix socket path defaults to /tmp/vmafx-sidecar.sock and is
# overridden by VMAFX_SIDECAR_SOCKET.  The vmafx-node and sidecar share a
# single emptyDir volume at /tmp (see deploy/helm/vmafx/templates/).
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import contextlib
import hashlib
import json
import logging
import os
import pathlib
import signal
import socket
import sys
import threading
from typing import Any

from .replay_buffer import ReplayBuffer, Sample
from .sgd_ema import SGDEMAConfig, SGDEMATrainer

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configuration from environment
# ---------------------------------------------------------------------------

_SOCKET_PATH = os.environ.get("VMAFX_SIDECAR_SOCKET", "/tmp/vmafx-sidecar.sock")
_BASE_MODEL_PATH = os.environ.get("VMAFX_BASE_MODEL_PATH", "")
_CHECKPOINT_DIR = os.environ.get("VMAFX_SIDECAR_CHECKPOINT_DIR", "/mnt/vmafx-models/online")
_REPLAY_BUFFER_CAPACITY = int(os.environ.get("VMAFX_SIDECAR_REPLAY_CAPACITY", "10000"))
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
                import onnx2torch  # type: ignore[import]

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
    batch_size:
        Mini-batch size for each gradient step.
    replay_mix_ratio:
        Fraction of each batch drawn from the replay buffer (0.0–1.0).
    config:
        SGDEMAConfig instance; defaults are used when None.
    """

    def __init__(
        self,
        n_features: int,
        base_model_path: str = "",
        checkpoint_dir: str = _CHECKPOINT_DIR,
        buffer_capacity: int = _REPLAY_BUFFER_CAPACITY,
        batch_size: int = _BATCH_SIZE,
        replay_mix_ratio: float = _REPLAY_MIX_RATIO,
        config: SGDEMAConfig | None = None,
    ) -> None:
        self._n_features = n_features
        self._checkpoint_dir = pathlib.Path(checkpoint_dir)
        self._checkpoint_dir.mkdir(parents=True, exist_ok=True)
        self._batch_size = batch_size
        self._replay_mix_ratio = replay_mix_ratio

        self._buffer = ReplayBuffer(capacity=buffer_capacity)
        model = _load_base_model(base_model_path, n_features)
        self._trainer = SGDEMATrainer(model, config)

        self._pending: list[Sample] = []  # samples received but not yet trained on
        self._lock = threading.Lock()
        # Separate lock guards the should_checkpoint() gate + counter increment +
        # filename choice + export so concurrent connection threads cannot race
        # the version number or export two checkpoints under the same name.
        self._checkpoint_lock = threading.Lock()
        self._checkpoint_counter: int = 0

    def ingest(self, features: list[float], true_score: float) -> dict[str, Any]:
        """Accept one (features, true_score) pair from the socket handler.

        Accumulates into the pending queue.  When the queue reaches
        ``_batch_size``, triggers one gradient step and flushes the queue.
        Returns a status dict for the JSON ACK response.

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

        with self._lock:
            self._pending.append(sample)
            self._buffer.push(sample.features, score)

            if len(self._pending) < self._batch_size:
                return {"ok": True, "step": self._trainer.step_count, "trained": False}

            # Build mixed batch: new samples + replay samples.
            batch = self._build_batch()
            self._pending.clear()

        # Step outside the lock — PyTorch backward pass is not reentrant anyway.
        loss = self._train_on_batch(batch)

        # Check checkpoint condition and export atomically under the checkpoint
        # lock — otherwise two connection threads can both pass should_checkpoint()
        # and collide on the same model_vNNNNNN.onnx version (R3-7).
        ckpt_path = self._maybe_export_checkpoint()

        return {
            "ok": True,
            "step": self._trainer.step_count,
            "trained": True,
            "loss": loss,
            "checkpoint": ckpt_path,
        }

    def status(self) -> dict[str, Any]:
        """Return a status snapshot for the /status endpoint."""
        buf_stats = self._buffer.stats
        return {
            "step_count": self._trainer.step_count,
            "buffer_size": buf_stats["current_size"],
            "buffer_capacity": buf_stats["capacity"],
            "total_pushed": buf_stats["total_pushed"],
            "total_evicted": buf_stats["total_evicted"],
            "checkpoint_counter": self._checkpoint_counter,
            "n_features": self._n_features,
        }

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _build_batch(self) -> list[Sample]:
        """Combine pending samples with a replay draw."""
        n_replay = int(self._batch_size * self._replay_mix_ratio)
        n_new = self._batch_size - n_replay
        new_samples = self._pending[-n_new:] if self._pending else []
        replay_samples = self._buffer.sample(n_replay)
        return new_samples + replay_samples

    def _train_on_batch(self, batch: list[Sample]) -> float:
        """Convert batch to tensors and call trainer.step()."""
        import torch

        if not batch:
            return 0.0
        feats = torch.tensor([s.features for s in batch], dtype=torch.float32)
        scores = torch.tensor([s.true_score for s in batch], dtype=torch.float32)
        return self._trainer.step(feats, scores)

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

    def _export_checkpoint(self) -> str:
        """Export EMA model to a versioned ONNX file.  Returns the path.

        Callers must hold ``_checkpoint_lock`` so the counter increment and the
        version-stamped filename are atomic across threads.
        """
        self._checkpoint_counter += 1
        ckpt_name = f"model_v{self._checkpoint_counter:06d}.onnx"
        ckpt_path = str(self._checkpoint_dir / ckpt_name)
        try:
            self._trainer.export_onnx(ckpt_path, n_features=self._n_features)
            _write_sha256_sidecar(ckpt_path)
            logger.info(
                "Checkpoint exported: %s (step=%d)",
                ckpt_path,
                self._trainer.step_count,
            )
        except Exception as exc:
            logger.error("Checkpoint export failed: %s", exc)
        return ckpt_path


# ---------------------------------------------------------------------------
# Unix-socket server
# ---------------------------------------------------------------------------


def _handle_connection(
    conn: socket.socket,
    trainer: OnlineTrainer,
) -> None:
    """Handle one client connection (vmafx-node goroutine).

    Reads newline-delimited JSON messages, calls trainer.ingest(), and
    writes JSON ACK responses.  The connection is closed when the client
    disconnects or sends malformed data.
    """
    addr = conn.getpeername() if conn.type != socket.AF_UNIX else "<unix>"
    logger.debug("Connection from %s", addr)
    buf = b""
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
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
                    # ValueError: malformed message (bad feature length / non-numeric
                    # score) or a ragged training batch. RuntimeError: shape-mismatched
                    # torch step. One bad message must return a structured error, not
                    # kill the per-connection daemon thread (R3-3).
                    err = json.dumps({"ok": False, "error": str(exc)}) + "\n"
                    conn.sendall(err.encode())
    except OSError as exc:
        logger.debug("Connection closed: %s", exc)
    finally:
        with contextlib.suppress(OSError):
            conn.close()
    logger.debug("Connection from %s closed", addr)


def run_server(
    socket_path: str = _SOCKET_PATH,
    trainer: OnlineTrainer | None = None,
    n_features: int = 80,
) -> None:
    """Start the Unix-socket server.  Blocks until SIGTERM/SIGINT.

    Parameters
    ----------
    socket_path:
        Path for the Unix domain socket.  The file is unlinked on clean shutdown.
    trainer:
        ``OnlineTrainer`` instance.  Constructed with defaults when None.
    n_features:
        Feature vector dimension.  Used only when *trainer* is None.
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

    # Remove stale socket from a previous run.
    with contextlib.suppress(FileNotFoundError):
        os.unlink(socket_path)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(socket_path)
    os.chmod(socket_path, 0o660)  # group-writable; Go node runs as same group
    srv.listen(16)
    srv.settimeout(1.0)  # allows the signal check below to fire promptly

    _stop = threading.Event()

    def _sighandler(signum: int, _frame: Any) -> None:
        logger.info("Received signal %d — initiating shutdown", signum)
        _stop.set()

    signal.signal(signal.SIGTERM, _sighandler)
    signal.signal(signal.SIGINT, _sighandler)

    logger.info("vmafx-sidecar listening on %s", socket_path)

    threads: list[threading.Thread] = []
    try:
        while not _stop.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                if _stop.is_set():
                    break
                raise
            t = threading.Thread(
                target=_handle_connection,
                args=(conn, trainer),
                daemon=True,
            )
            t.start()
            threads.append(t)
            # Prune dead threads to avoid unbounded list growth.
            threads = [tt for tt in threads if tt.is_alive()]
    finally:
        srv.close()
        with contextlib.suppress(FileNotFoundError):
            os.unlink(socket_path)
        logger.info("vmafx-sidecar stopped")


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
