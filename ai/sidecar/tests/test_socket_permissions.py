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
import queue
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest.mock as mock
from typing import Generator, NamedTuple

import pytest

from ai.sidecar import online_trainer as ot_mod

pytestmark = pytest.mark.skipif(
    os.name != "posix",
    reason="Unix socket permissions and peer credentials require POSIX",
)


class _PeerProcess(NamedTuple):
    uid: int
    gid: int
    process: subprocess.Popen[str]
    uses_user_namespace: bool


def _subid_start(path: pathlib.Path, username: str) -> int | None:
    """Return the first subordinate ID assigned to *username*."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    for line in lines:
        name, separator, remainder = line.partition(":")
        if name == username and separator:
            start, _, _ = remainder.partition(":")
            return int(start)
    return None


def _peer_command(script: str) -> tuple[list[str], int, int, bool]:
    """Build a real different-UID/same-GID peer command or skip unsupported hosts."""
    server_uid = os.geteuid()
    server_gid = os.getegid()
    if server_uid == 0:
        target_uid = 65534
        drop = (
            "import os\n"
            "os.setgroups([])\n"
            f"os.setgid({server_gid})\n"
            f"os.setuid({target_uid})\n"
        )
        return [sys.executable, "-c", drop + script], target_uid, server_gid, False

    if not sys.platform.startswith("linux"):
        pytest.skip("different-UID replay requires root or Linux user namespaces")
    required = {name: shutil.which(name) for name in ("unshare", "newuidmap", "newgidmap")}
    if not all(required.values()):
        pytest.skip("different-UID replay requires unshare, newuidmap, and newgidmap")

    import pwd

    username = pwd.getpwuid(server_uid).pw_name
    subuid = _subid_start(pathlib.Path("/etc/subuid"), username)
    subgid = _subid_start(pathlib.Path("/etc/subgid"), username)
    if subuid is None or subgid is None:
        pytest.skip(f"no subordinate UID/GID range is assigned to {username}")
    base_python = getattr(sys, "_base_executable", sys.executable)
    command = [
        required["unshare"],
        "--user",
        "--map-users",
        f"0:{server_uid}:1",
        "--map-users",
        f"1:{subuid}:1",
        "--map-groups",
        f"0:{server_gid}:1",
        "--map-groups",
        f"1:{subgid}:1",
        "--setuid",
        "1",
        "--setgid",
        "0",
        "--fork",
        base_python,
        "-c",
        script,
    ]
    return command, subuid, server_gid, True


def _spawn_peer(script: str) -> _PeerProcess:
    command, uid, gid, uses_user_namespace = _peer_command(script)
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as exc:
        pytest.skip(f"could not start different-UID peer: {exc}")
    return _PeerProcess(uid, gid, process, uses_user_namespace)


def _finish_peer(peer: _PeerProcess) -> str:
    try:
        stdout, stderr = peer.process.communicate(timeout=5.0)
    except subprocess.TimeoutExpired:
        peer.process.kill()
        peer.process.communicate()
        pytest.fail("different-UID peer did not terminate within five seconds")
    if peer.process.returncode and peer.uses_user_namespace:
        if any(marker in stderr for marker in ("unshare:", "newuidmap:", "newgidmap:")):
            pytest.skip(f"user-namespace mapping is unavailable: {stderr.strip()}")
    assert peer.process.returncode == 0, f"different-UID peer failed: {stderr}"
    return stdout


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

    def test_server_bind_failure_does_not_mask_exception(self, tmp_path: pathlib.Path) -> None:
        """Failure to bind (e.g. non-existent directory) raises OSError directly, not UnboundLocalError."""
        invalid_path = str(tmp_path / "missing_dir" / "test.sock")
        dummy_trainer = mock.MagicMock()
        stop_event = threading.Event()
        with pytest.raises(OSError) as exc_info:
            ot_mod.run_server(
                socket_path=invalid_path,
                trainer=dummy_trainer,
                stop_event=stop_event,
            )
        assert not isinstance(exc_info.value, UnboundLocalError)

    def test_server_held_clients_terminate_promptly_on_shutdown(
        self, tmp_path: pathlib.Path
    ) -> None:
        """Held client connections blocked on recv are closed on shutdown without delaying server stop."""
        sock_path = str(tmp_path / "held_clients.sock")
        dummy_trainer = mock.MagicMock()
        stop_event = threading.Event()

        server_thread = threading.Thread(
            target=ot_mod.run_server,
            kwargs={
                "socket_path": sock_path,
                "trainer": dummy_trainer,
                "stop_event": stop_event,
            },
            daemon=True,
        )
        server_thread.start()

        # Wait until server is listening
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                try:
                    probe.connect(sock_path)
                    break
                except OSError:
                    time.sleep(0.01)

        # Open 3 client connections that send nothing and block
        clients = [socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) for _ in range(3)]
        for c in clients:
            c.connect(sock_path)

        # Signal shutdown and measure duration
        start_time = time.monotonic()
        stop_event.set()
        # Wake up accept loop
        with contextlib.suppress(OSError):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as wake:
                wake.connect(sock_path)

        server_thread.join(timeout=2.0)
        shutdown_elapsed = time.monotonic() - start_time

        assert not server_thread.is_alive(), "Server thread did not shut down promptly"
        assert shutdown_elapsed < 1.5, f"Shutdown took too long: {shutdown_elapsed:.2f}s"

        # Verify all clients are disconnected (either clean EOF or ConnectionResetError)
        for c in clients:
            try:
                data = c.recv(1024)
                assert (
                    data == b""
                ), "Client connection should have received EOF upon server shutdown"
            except ConnectionResetError:
                pass  # Connection was reset/terminated by server shutdown
            finally:
                c.close()

    def test_server_registration_race_between_accept_and_shutdown(
        self, tmp_path: pathlib.Path
    ) -> None:
        """Races between accept, handler thread registration, and shutdown must never leak connections or hang.

        If thread scheduling delays worker thread startup in _handle_connection, the accepted
        socket is already registered in active_conns by run_server under conns_lock, ensuring
        deterministic and prompt shutdown (< 1.5s) without thread join timeouts.
        """
        sock_path = str(tmp_path / "race_test.sock")
        dummy_trainer = mock.MagicMock()
        stop_event = threading.Event()

        orig_handle = ot_mod._handle_connection

        def delayed_handle(*args, **kwargs):
            # Inject simulated thread scheduling latency before connection loop
            time.sleep(0.15)
            return orig_handle(*args, **kwargs)

        with mock.patch("ai.sidecar.online_trainer._handle_connection", side_effect=delayed_handle):
            server_thread = threading.Thread(
                target=ot_mod.run_server,
                kwargs={
                    "socket_path": sock_path,
                    "trainer": dummy_trainer,
                    "stop_event": stop_event,
                },
                daemon=True,
            )
            server_thread.start()

            # Wait until server is listening
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                    try:
                        probe.connect(sock_path)
                        break
                    except OSError:
                        time.sleep(0.01)

            # Connect client
            c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            c.connect(sock_path)

            # Allow server to accept conn and register it before worker thread wakes up
            time.sleep(0.03)

            # Trigger shutdown while worker thread is still in delayed_handle sleep
            start_time = time.monotonic()
            stop_event.set()
            with contextlib.suppress(OSError):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as wake:
                    wake.connect(sock_path)

            server_thread.join(timeout=2.0)
            shutdown_elapsed = time.monotonic() - start_time

            assert not server_thread.is_alive(), "Server thread hung during registration race"
            # Red-capable assertion: without pre-spawn registration, worker thread join hung for >= 1.0s
            assert (
                shutdown_elapsed < 0.6
            ), f"Shutdown took too long during race: {shutdown_elapsed:.2f}s"

            with contextlib.suppress(OSError):
                c.close()

    def test_server_accept_after_stop_closes_connection(self, tmp_path: pathlib.Path) -> None:
        """A connection accepted after stop is cleanly closed by the registry."""
        sock_path = str(tmp_path / "stop_after_accept.sock")
        dummy_trainer = mock.MagicMock()
        stop_event = threading.Event()

        server_thread = threading.Thread(
            target=ot_mod.run_server,
            kwargs={
                "socket_path": sock_path,
                "trainer": dummy_trainer,
                "stop_event": stop_event,
            },
            daemon=True,
        )
        server_thread.start()

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                try:
                    probe.connect(sock_path)
                    break
                except OSError:
                    time.sleep(0.01)

        # Signal stop
        stop_event.set()

        # Connect client to wake up accept
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.connect(sock_path)
            data = client.recv(1024)
            assert data == b""
        except OSError:
            pass
        finally:
            client.close()

        server_thread.join(timeout=2.0)
        assert not server_thread.is_alive()

    @pytest.fixture
    def shared_ipc_dir(self) -> Generator[pathlib.Path, None, None]:
        """Provide a group-accessible directory in /tmp for cross-UID/same-GID IPC testing."""
        td = tempfile.mkdtemp(prefix="vmafx_ipc_", dir="/tmp")
        os.chmod(td, 0o770)
        try:
            yield pathlib.Path(td)
        finally:
            shutil.rmtree(td, ignore_errors=True)

    @pytest.mark.skipif(
        not hasattr(socket, "SO_PEERCRED"),
        reason="kernel peer-credential assertions require Linux SO_PEERCRED",
    )
    def test_production_server_accepts_different_uid_same_gid_peer(
        self, monkeypatch: pytest.MonkeyPatch, shared_ipc_dir: pathlib.Path
    ) -> None:
        """Exercise production IPC and assert the kernel-observed peer credentials."""
        sock_path = str(shared_ipc_dir / "different_uid.sock")
        credentials: queue.Queue[tuple[int, int, int]] = queue.Queue()
        original_handler = ot_mod._handle_connection

        def capture_credentials(conn, trainer, registry=None):
            raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
            credentials.put(struct.unpack("3i", raw))
            return original_handler(conn, trainer, registry)

        monkeypatch.setattr(ot_mod, "_handle_connection", capture_credentials)
        child_code = f"""
import json, socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect({sock_path!r})
msg = {{"job_id": "different-uid", "features": [0.2] * 8, "true_score": 90.0}}
s.sendall((json.dumps(msg) + "\\n").encode())
print(s.recv(4096).decode().strip())
s.close()
"""
        with self._running_server(sock_path):
            peer = _spawn_peer(child_code)
            output = _finish_peer(peer)

        observed = [credentials.get_nowait() for _ in range(credentials.qsize())]
        assert any(uid == peer.uid and gid == peer.gid for _, uid, gid in observed)
        assert peer.uid != os.geteuid()
        assert peer.gid == os.getegid()
        ack = json.loads(output)
        assert ack["ok"] is True
        assert ack["job_id"] == "different-uid"

    @pytest.mark.parametrize("bad_mode", (0o644, 0o600))
    def test_same_group_different_uid_peer_requires_group_write(
        self, shared_ipc_dir: pathlib.Path, bad_mode: int
    ) -> None:
        """A real same-GID peer cannot connect without the socket's group-write bit."""
        sock_path = str(shared_ipc_dir / f"bad_{bad_mode:o}.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        os.chmod(sock_path, bad_mode)
        server.listen(1)
        child_code = f"""
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect({sock_path!r})
    print("unexpected-connect")
except PermissionError:
    print("expected-eacces")
finally:
    s.close()
"""
        try:
            output = _finish_peer(_spawn_peer(child_code))
            assert output.strip() == "expected-eacces"
        finally:
            server.close()
            with contextlib.suppress(FileNotFoundError):
                os.unlink(sock_path)


class TestConnectionRegistry:
    """Unit tests for the private connection-registry lifecycle abstraction."""

    def test_registry_registration_and_discard(self) -> None:
        registry = ot_mod._ConnectionRegistry()
        stop_event = threading.Event()
        assert len(registry) == 0

        s1, s2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            assert registry.register_if_active(s1, stop_event) is True
            assert len(registry) == 1
            assert registry.register_if_active(s2, stop_event) is True
            assert len(registry) == 2

            registry.discard(s1)
            assert len(registry) == 1
            registry.discard(s2)
            assert len(registry) == 0
        finally:
            s1.close()
            s2.close()

    def test_registry_close_all_terminates_all(self) -> None:
        registry = ot_mod._ConnectionRegistry()
        stop_event = threading.Event()
        s1, c1 = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        s2, c2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            assert registry.register_if_active(s1, stop_event) is True
            assert registry.register_if_active(s2, stop_event) is True
            assert len(registry) == 2

            registry.close_all()
            assert len(registry) == 0

            assert c1.recv(1024) == b""
            assert c2.recv(1024) == b""
        finally:
            c1.close()
            c2.close()

    def test_registry_register_if_active_prevents_leaks_on_stop(self) -> None:
        registry = ot_mod._ConnectionRegistry()
        stop_event = threading.Event()

        s1, c1 = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            assert registry.register_if_active(s1, stop_event) is True
            assert len(registry) == 1
        finally:
            registry.discard(s1)
            s1.close()
            c1.close()

        stop_event.set()
        s2, c2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            assert registry.register_if_active(s2, stop_event) is False
            assert len(registry) == 0
            assert c2.recv(1024) == b""
        finally:
            c2.close()
