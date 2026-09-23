# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_socket_permissions.py — unit tests for Unix domain socket
# file ownership, permissions, and group sharing semantics in online_trainer.py.
#
# Verifies mode 0o660 requirements, zero world access, user/group read-write access,
# ownership attributes, and proves why Semgrep alert 946 is an intentional group-shared
# IPC artifact rather than an over-permissive defect.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import contextlib
import json
import os
import pathlib
import socket
import stat
import threading
import time
import unittest.mock as mock
from typing import Generator

from ai.sidecar import online_trainer as ot_mod


class TestSocketPermissionsAndOwnership:
    """Red-capable regression tests for Unix domain socket file mode and ownership."""

    @contextlib.contextmanager
    def _running_server(self, sock_path: str) -> Generator[threading.Thread, None, None]:
        """Start run_server in a daemon thread and ensure clean shutdown."""
        dummy_trainer = mock.MagicMock()
        # Mock status() and ingest() so protocol messages succeed
        dummy_trainer.status.return_value = {
            "step_count": 0,
            "buffer_size": 0,
            "buffer_capacity": 1000,
            "total_pushed": 0,
            "total_evicted": 0,
            "checkpoint_counter": 0,
            "n_features": 8,
        }
        dummy_trainer.ingest.return_value = {
            "ok": True,
            "trained": False,
            "step": 1,
        }

        stop_event = threading.Event()
        thread = threading.Thread(
            target=ot_mod.run_server,
            kwargs={
                "socket_path": sock_path,
                "trainer": dummy_trainer,
                "stop_event": stop_event,
            },
            daemon=True,
        )
        thread.start()

        # Wait for the server socket to be actively listening
        deadline = time.monotonic() + 3.0
        ready = False
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                try:
                    probe.connect(sock_path)
                    ready = True
                    break
                except OSError:
                    time.sleep(0.01)

        assert ready, f"Socket {sock_path} was not ready within deadline"

        try:
            yield thread
        finally:
            stop_event.set()
            # Wake up accept loop immediately
            with contextlib.suppress(OSError):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.connect(sock_path)
            thread.join(timeout=3.0)
            assert not thread.is_alive(), "Server thread failed to terminate cleanly"

    def test_server_socket_mode_exact_0o660(self, tmp_path: pathlib.Path) -> None:
        """The Unix domain socket must have permission mode exactly 0o660 (rw-rw----)."""
        sock_path = str(tmp_path / "vmafx-sidecar.sock")
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            assert stat.S_ISSOCK(st.st_mode), "Path must be a Unix domain socket"
            mode = stat.S_IMODE(st.st_mode)
            # Red-capable assertion: fails if mode is 0o644, 0o600, 0o666, 0o755, etc.
            assert mode == 0o660, f"Expected socket mode 0o660, got {oct(mode)}"

        # Socket must be cleaned up on server exit
        assert not os.path.exists(sock_path)

    def test_server_socket_permissions_bits_breakdown(self, tmp_path: pathlib.Path) -> None:
        """Verify explicit user rw, group rw, and zero other/world permission bits."""
        sock_path = str(tmp_path / "vmafx-sidecar.sock")
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            mode = stat.S_IMODE(st.st_mode)

            # User permissions: must have read and write (0o600), no execute
            assert (
                mode & 0o700
            ) == 0o600, f"User bits must be rw- (0o600), got {oct(mode & 0o700)}"

            # Group permissions: must have read and write (0o060), no execute
            # Required for Go node peer in the same group to connect and send samples.
            assert (
                mode & 0o070
            ) == 0o060, f"Group bits must be rw- (0o060), got {oct(mode & 0o070)}"

            # World / Other permissions: MUST be strictly 0 (---)
            # No world access is permitted; prevents unauthorized local users.
            assert (
                mode & 0o007
            ) == 0o000, f"World bits must be --- (0o000), got {oct(mode & 0o007)}"

            # No execute bits on socket file
            assert (mode & 0o111) == 0o000, "Socket file must not have execute bits set"

    def test_server_socket_ownership(self, tmp_path: pathlib.Path) -> None:
        """The Unix domain socket must be owned by current process EUID and EGID."""
        sock_path = str(tmp_path / "vmafx-sidecar.sock")
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            assert st.st_uid == os.geteuid(), "Socket UID must match process EUID"
            assert st.st_gid == os.getegid(), "Socket GID must match process EGID"

    def test_red_capable_mode_regression_guards(self) -> None:
        """Theoretical and mathematical verification of mode boundary invariants.

        Proves why alternative permissions (0o644, 0o600, 0o666) fail security
        or operational requirements.
        """
        # Semgrep recommendation (0o644):
        # Lacks S_IWGRP (group write), which prevents the same-group peer from connecting
        mode_644 = 0o644
        assert not (mode_644 & stat.S_IWGRP), "0o644 lacks group write, breaking Go peer IPC"
        assert mode_644 & stat.S_IROTH, "0o644 permits world read, leaking presence to all users"

        # Overly restrictive user-only mode (0o600):
        # Lacks all group permissions, preventing multi-container pod IPC across UIDs
        mode_600 = 0o600
        assert not (
            mode_600 & (stat.S_IRGRP | stat.S_IWGRP)
        ), "0o600 denies group access completely"

        # Over-permissive world mode (0o666 / 0o777):
        # Grants world access, violating least-privilege security policy
        mode_666 = 0o666
        assert (mode_666 & 0o007) != 0, "0o666 allows world read/write"

        # Production mode (0o660):
        # Exactly satisfies group IPC write + zero world exposure
        prod_mode = 0o660
        assert (prod_mode & stat.S_IRUSR) and (prod_mode & stat.S_IWUSR)
        assert (prod_mode & stat.S_IRGRP) and (prod_mode & stat.S_IWGRP)
        assert (prod_mode & 0o007) == 0

    def test_socket_recreation_cleans_stale_socket_with_wrong_permissions(
        self, tmp_path: pathlib.Path
    ) -> None:
        """A pre-existing stale socket with different permissions must be cleanly replaced."""
        sock_path = str(tmp_path / "stale.sock")
        # Pre-create a stale socket file with wrong mode (0o600)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale_srv:
            stale_srv.bind(sock_path)
            os.chmod(sock_path, 0o600)

        assert os.path.exists(sock_path)
        assert stat.S_IMODE(os.stat(sock_path).st_mode) == 0o600

        # Run server over the same path; it must unlink stale and set 0o660
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            assert stat.S_IMODE(st.st_mode) == 0o660

    def test_ipc_connection_succeeds_under_0o660(self, tmp_path: pathlib.Path) -> None:
        """Connecting client can send messages and receive ACK over 0o660 socket."""
        sock_path = str(tmp_path / "ipc_test.sock")
        with self._running_server(sock_path):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.connect(sock_path)

                message = {
                    "job_id": "job-1001",
                    "features": [0.1] * 8,
                    "true_score": 85.0,
                }
                client.sendall((json.dumps(message) + "\n").encode())

                buf = b""
                while b"\n" not in buf:
                    chunk = client.recv(1024)
                    if not chunk:
                        break
                    buf += chunk

                ack = json.loads(buf.split(b"\n", 1)[0])
                assert ack.get("ok") is True
                assert ack.get("job_id") == "job-1001"
