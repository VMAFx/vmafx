# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_online_trainer.py — unit tests for OnlineTrainer
# and the online_trainer module helpers.
#
# All tests run CPU-only and require no corpus, no GPU, and no pre-trained
# model files.  PyTorch is required; the test module is skipped when absent.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import json
import pathlib
import socket
import threading
import time

import pytest

torch = pytest.importorskip("torch", reason="PyTorch not installed — skipping OnlineTrainer tests")

from ai.sidecar import online_trainer as ot_mod  # noqa: E402
from ai.sidecar.online_trainer import (  # noqa: E402
    OnlineTrainer,
    _build_fallback_model,
    _load_base_model,
    _write_sha256_sidecar,
)
from ai.sidecar.sgd_ema import SGDEMAConfig  # noqa: E402

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

N_FEATURES = 8  # small for fast CPU runs


# ---------------------------------------------------------------------------
# _build_fallback_model
# ---------------------------------------------------------------------------


class TestBuildFallbackModel:
    def test_returns_nn_module(self) -> None:
        import torch.nn as nn

        model = _build_fallback_model(N_FEATURES)
        assert isinstance(model, nn.Module)

    def test_output_shape_is_batch_by_1(self) -> None:
        feats = torch.randn(4, N_FEATURES)
        model = _build_fallback_model(N_FEATURES)
        out = model(feats)
        assert out.shape == (4, 1)

    def test_different_n_features_accepted(self) -> None:
        for n in (6, 16, 80):
            model = _build_fallback_model(n)
            out = model(torch.randn(2, n))
            assert out.shape == (2, 1)


# ---------------------------------------------------------------------------
# _load_base_model
# ---------------------------------------------------------------------------


class TestLoadBaseModel:
    def test_empty_path_returns_fallback(self) -> None:
        import torch.nn as nn

        model = _load_base_model("", N_FEATURES)
        assert isinstance(model, nn.Module)

    def test_absent_path_returns_fallback(self, tmp_path: pathlib.Path) -> None:
        import torch.nn as nn

        model = _load_base_model(str(tmp_path / "no_such_model.pt"), N_FEATURES)
        assert isinstance(model, nn.Module)

    def test_loads_pytorch_state_dict(self, tmp_path: pathlib.Path) -> None:
        import torch
        import torch.nn as nn

        # Build a known-weight model and save its state-dict.
        original = _build_fallback_model(N_FEATURES)
        state_path = tmp_path / "model.pt"
        torch.save(original.state_dict(), str(state_path))

        loaded = _load_base_model(str(state_path), N_FEATURES)
        assert isinstance(loaded, nn.Module)
        # Weights should match the saved state.
        for p_orig, p_loaded in zip(original.parameters(), loaded.parameters(), strict=True):
            assert torch.allclose(p_orig.data, p_loaded.data)

    def test_corrupt_pt_file_returns_fallback(self, tmp_path: pathlib.Path) -> None:
        import torch.nn as nn

        corrupt = tmp_path / "corrupt.pt"
        corrupt.write_bytes(b"not a valid pytorch file")
        model = _load_base_model(str(corrupt), N_FEATURES)
        # Should fall back to untrained MLP rather than raising.
        assert isinstance(model, nn.Module)


# ---------------------------------------------------------------------------
# _write_sha256_sidecar
# ---------------------------------------------------------------------------


class TestWriteSha256Sidecar:
    def test_creates_sha256_file_beside_onnx(self, tmp_path: pathlib.Path) -> None:
        onnx_path = tmp_path / "model.onnx"
        onnx_path.write_bytes(b"fake onnx content")
        _write_sha256_sidecar(str(onnx_path))
        sidecar = pathlib.Path(str(onnx_path) + ".sha256")
        assert sidecar.exists()

    def test_sha256_file_contains_valid_hex(self, tmp_path: pathlib.Path) -> None:
        onnx_path = tmp_path / "model.onnx"
        onnx_path.write_bytes(b"hello world")
        _write_sha256_sidecar(str(onnx_path))
        sidecar_text = pathlib.Path(str(onnx_path) + ".sha256").read_text().strip()
        # SHA-256 hex digest is exactly 64 hex characters.
        assert len(sidecar_text) == 64
        assert all(c in "0123456789abcdef" for c in sidecar_text)

    def test_sha256_matches_known_value(self, tmp_path: pathlib.Path) -> None:
        import hashlib

        content = b"deterministic content"
        onnx_path = tmp_path / "model.onnx"
        onnx_path.write_bytes(content)
        _write_sha256_sidecar(str(onnx_path))
        expected = hashlib.sha256(content).hexdigest()
        sidecar_text = pathlib.Path(str(onnx_path) + ".sha256").read_text().strip()
        assert sidecar_text == expected

    def test_write_is_atomic_no_partial_file(self, tmp_path: pathlib.Path) -> None:
        """The .tmp.sha256 intermediate must not exist after the call."""
        onnx_path = tmp_path / "model.onnx"
        onnx_path.write_bytes(b"x")
        _write_sha256_sidecar(str(onnx_path))
        tmp_sidecar = pathlib.Path(str(onnx_path) + ".sha256.tmp")
        assert not tmp_sidecar.exists()


# ---------------------------------------------------------------------------
# OnlineTrainer construction
# ---------------------------------------------------------------------------


def _fast_config() -> SGDEMAConfig:
    """SGDEMAConfig with tiny thresholds so tests don't have to wait."""
    return SGDEMAConfig(
        lr=1e-3,
        warmup_steps=0,
        checkpoint_interval_s=0.0,
        min_samples_per_checkpoint=0,
    )


class TestOnlineTrainerInit:
    def test_constructs_without_error(self, tmp_path: pathlib.Path) -> None:
        trainer = OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path / "ckpts"),
            config=_fast_config(),
        )
        status = trainer.status()
        assert status["step_count"] == 0
        assert status["buffer_size"] == 0
        assert status["n_features"] == N_FEATURES

    def test_checkpoint_dir_is_created(self, tmp_path: pathlib.Path) -> None:
        ckpt_dir = tmp_path / "new_dir" / "nested"
        OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(ckpt_dir),
            config=_fast_config(),
        )
        assert ckpt_dir.is_dir()

    def test_custom_buffer_capacity(self, tmp_path: pathlib.Path) -> None:
        trainer = OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path),
            buffer_capacity=256,
            config=_fast_config(),
        )
        assert trainer.status()["buffer_capacity"] == 256


# ---------------------------------------------------------------------------
# OnlineTrainer.ingest — accumulation and gradient step
# ---------------------------------------------------------------------------


class TestOnlineTrainerIngest:
    def _trainer(self, tmp_path: pathlib.Path, batch_size: int = 4) -> OnlineTrainer:
        return OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path / "ckpts"),
            batch_size=batch_size,
            config=_fast_config(),
        )

    def test_ingest_below_batch_does_not_train(self, tmp_path: pathlib.Path) -> None:
        trainer = self._trainer(tmp_path, batch_size=8)
        features = [float(i) for i in range(N_FEATURES)]
        result = trainer.ingest(features, 50.0)
        assert result["ok"] is True
        assert result["trained"] is False

    def test_ingest_fills_buffer(self, tmp_path: pathlib.Path) -> None:
        trainer = self._trainer(tmp_path, batch_size=8)
        features = [float(i) for i in range(N_FEATURES)]
        for _ in range(3):
            trainer.ingest(features, 50.0)
        assert trainer.status()["buffer_size"] == 3

    def test_ingest_triggers_step_at_batch_size(self, tmp_path: pathlib.Path) -> None:
        batch_size = 4
        trainer = self._trainer(tmp_path, batch_size=batch_size)
        features = [float(i) for i in range(N_FEATURES)]
        result = None
        for i in range(batch_size):
            result = trainer.ingest(features, float(i * 10))
        assert result is not None
        assert result["ok"] is True
        assert result["trained"] is True
        assert "loss" in result
        assert isinstance(result["loss"], float)
        assert result["step"] == 1

    def test_ingest_increments_step_on_each_batch(self, tmp_path: pathlib.Path) -> None:
        batch_size = 2
        trainer = self._trainer(tmp_path, batch_size=batch_size)
        features = [0.5] * N_FEATURES
        for _ in range(batch_size * 3):
            trainer.ingest(features, 75.0)
        assert trainer.status()["step_count"] == 3

    def test_status_total_pushed_tracks_all_ingested(self, tmp_path: pathlib.Path) -> None:
        trainer = self._trainer(tmp_path, batch_size=8)
        features = [1.0] * N_FEATURES
        for _ in range(5):
            trainer.ingest(features, 60.0)
        assert trainer.status()["total_pushed"] == 5

    def test_ingest_returns_checkpoint_path_when_condition_met(
        self, tmp_path: pathlib.Path
    ) -> None:
        batch_size = 2
        trainer = self._trainer(tmp_path, batch_size=batch_size)
        features = [1.0] * N_FEATURES
        result = None
        for i in range(batch_size):
            result = trainer.ingest(features, float(i))
        assert result is not None
        # checkpoint_interval_s=0 and min_samples=0 → should_checkpoint() is True
        ckpt = result.get("checkpoint")
        assert ckpt is not None
        assert pathlib.Path(ckpt).exists()

    def test_checkpoint_file_has_sha256_sidecar(self, tmp_path: pathlib.Path) -> None:
        batch_size = 2
        trainer = self._trainer(tmp_path, batch_size=batch_size)
        features = [1.0] * N_FEATURES
        result = None
        for i in range(batch_size):
            result = trainer.ingest(features, float(i))
        assert result is not None
        ckpt = result.get("checkpoint")
        assert ckpt is not None
        sidecar = pathlib.Path(ckpt + ".sha256")
        assert sidecar.exists()


# ---------------------------------------------------------------------------
# OnlineTrainer.status
# ---------------------------------------------------------------------------


class TestOnlineTrainerStatus:
    def test_status_keys_present(self, tmp_path: pathlib.Path) -> None:
        trainer = OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path),
            config=_fast_config(),
        )
        status = trainer.status()
        for key in (
            "step_count",
            "buffer_size",
            "buffer_capacity",
            "total_pushed",
            "total_evicted",
            "checkpoint_counter",
            "n_features",
        ):
            assert key in status, f"Key {key!r} missing from status()"

    def test_checkpoint_counter_increments(self, tmp_path: pathlib.Path) -> None:
        batch_size = 2
        trainer = OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path / "ckpts"),
            batch_size=batch_size,
            config=_fast_config(),
        )
        features = [0.5] * N_FEATURES
        for _ in range(batch_size * 3):
            trainer.ingest(features, 50.0)
        assert trainer.status()["checkpoint_counter"] >= 1


# ---------------------------------------------------------------------------
# _handle_connection — protocol-level tests via loopback sockets
# ---------------------------------------------------------------------------


def _make_trainer(tmp_path: pathlib.Path) -> OnlineTrainer:
    return OnlineTrainer(
        n_features=N_FEATURES,
        checkpoint_dir=str(tmp_path / "ckpts"),
        batch_size=64,  # large batch so training doesn't fire every message
        config=_fast_config(),
    )


def _send_recv(sock: socket.socket, payload: dict) -> dict:
    """Send a newline-delimited JSON message and read the ACK."""
    sock.sendall((json.dumps(payload) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("socket closed before ACK")
        buf += chunk
    return json.loads(buf.split(b"\n", 1)[0])


class TestHandleConnection:
    """Exercise _handle_connection over a real socketpair."""

    def _run_server_thread(
        self, server_sock: socket.socket, trainer: OnlineTrainer
    ) -> threading.Thread:
        t = threading.Thread(
            target=ot_mod._handle_connection,
            args=(server_sock, trainer),
            daemon=True,
        )
        t.start()
        return t

    def _socketpair(self) -> tuple[socket.socket, socket.socket]:
        return socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)

    def test_valid_message_returns_ok_true(self, tmp_path: pathlib.Path) -> None:
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        payload = {
            "job_id": "job-001",
            "features": [float(i) for i in range(N_FEATURES)],
            "true_score": 72.5,
        }
        ack = _send_recv(client_sock, payload)
        assert ack["ok"] is True
        assert "step" in ack
        client_sock.close()

    def test_job_id_echoed_in_ack(self, tmp_path: pathlib.Path) -> None:
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        payload = {
            "job_id": "my-unique-job",
            "features": [1.0] * N_FEATURES,
            "true_score": 55.0,
        }
        ack = _send_recv(client_sock, payload)
        assert ack.get("job_id") == "my-unique-job"
        client_sock.close()

    def test_missing_features_key_returns_ok_false(self, tmp_path: pathlib.Path) -> None:
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        payload = {"job_id": "bad", "true_score": 50.0}  # no "features"
        ack = _send_recv(client_sock, payload)
        assert ack["ok"] is False
        assert "error" in ack
        client_sock.close()

    def test_malformed_json_returns_ok_false(self, tmp_path: pathlib.Path) -> None:
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        client_sock.sendall(b"not json at all\n")
        buf = b""
        while b"\n" not in buf:
            chunk = client_sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        ack = json.loads(buf.split(b"\n", 1)[0])
        assert ack["ok"] is False
        client_sock.close()

    def test_empty_line_is_silently_skipped(self, tmp_path: pathlib.Path) -> None:
        """Blank lines between messages are ignored; next valid message still ACKd."""
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        # Send an empty line followed by a valid message.
        valid = {
            "job_id": "j2",
            "features": [0.5] * N_FEATURES,
            "true_score": 80.0,
        }
        client_sock.sendall(b"\n")
        ack = _send_recv(client_sock, valid)
        assert ack["ok"] is True
        client_sock.close()

    def test_multiple_messages_on_one_connection(self, tmp_path: pathlib.Path) -> None:
        trainer = _make_trainer(tmp_path)
        client_sock, server_sock = self._socketpair()
        self._run_server_thread(server_sock, trainer)

        for i in range(5):
            payload = {
                "job_id": f"j{i}",
                "features": [float(i)] * N_FEATURES,
                "true_score": float(i * 10),
            }
            ack = _send_recv(client_sock, payload)
            assert ack["ok"] is True
        client_sock.close()


# ---------------------------------------------------------------------------
# run_server smoke test — start + immediate shutdown via signal
# ---------------------------------------------------------------------------


class TestRunServer:
    def test_run_server_starts_and_stops_cleanly(self, tmp_path: pathlib.Path) -> None:
        """run_server must bind the socket, accept connections, and stop on event."""
        sock_path = str(tmp_path / "test.sock")
        trainer = OnlineTrainer(
            n_features=N_FEATURES,
            checkpoint_dir=str(tmp_path / "ckpts"),
            config=_fast_config(),
        )

        errors: list[Exception] = []

        # Patch threading.Event inside run_server to give us control of the stop flag.
        original_event_class = threading.Event

        class ControlledEvent:
            """Mimics threading.Event but stops after the socket is accepting."""

            def __init__(self) -> None:
                self._inner = original_event_class()
                # Let the server loop one iteration, then stop.
                self._fire_after = 0.1

            def set(self) -> None:
                self._inner.set()

            def is_set(self) -> bool:
                # Stop after a short delay.
                if not self._inner.is_set() and time.monotonic() > self._start + self._fire_after:
                    self._inner.set()
                return self._inner.is_set()

            # Called just before the first accept() — record start time.
            @property
            def _start(self):
                if not hasattr(self, "_t0"):
                    self._t0 = time.monotonic()
                return self._t0

        import unittest.mock as mock

        with mock.patch.object(threading, "Event", ControlledEvent):
            try:
                ot_mod.run_server(socket_path=sock_path, trainer=trainer)
            except Exception as exc:
                errors.append(exc)

        assert not errors, f"run_server raised: {errors}"
        # Socket file cleaned up after exit.
        assert not pathlib.Path(sock_path).exists()
