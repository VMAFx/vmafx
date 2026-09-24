# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_socket_permissions.py — unit tests for Unix domain socket
# file ownership, permissions, and group sharing semantics in online_trainer.py.
#
# Verifies owner-only socket permissions, ownership attributes, safe stale-socket
# recovery, and pathname ownership across adversarial lifecycle transitions.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import contextlib
import errno
import json
import os
import pathlib
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest.mock as mock
from typing import Any, Generator, NamedTuple

import pytest

from ai.sidecar import online_trainer as ot_mod

pytestmark = pytest.mark.skipif(
    os.name != "posix",
    reason="Unix socket filesystem permissions require POSIX",
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
    unshare = shutil.which("unshare")
    newuidmap = shutil.which("newuidmap")
    newgidmap = shutil.which("newgidmap")
    if unshare is None or newuidmap is None or newgidmap is None:
        pytest.skip("different-UID replay requires unshare, newuidmap, and newgidmap")

    import pwd

    username = pwd.getpwuid(server_uid).pw_name
    subuid = _subid_start(pathlib.Path("/etc/subuid"), username)
    subgid = _subid_start(pathlib.Path("/etc/subgid"), username)
    if subuid is None or subgid is None:
        pytest.skip(f"no subordinate UID/GID range is assigned to {username}")
    base_python = getattr(sys, "_base_executable", sys.executable)
    command = [
        unshare,
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
    """Regression tests for Unix socket permissions and pathname ownership."""

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

    def test_server_socket_mode_exact_0o600(self, tmp_path: pathlib.Path) -> None:
        """The Unix domain socket defaults to owner-only mode 0o600."""
        sock_path = str(tmp_path / "vmafx-sidecar.sock")
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            assert stat.S_ISSOCK(st.st_mode), "Path must be a Unix domain socket"
            mode = stat.S_IMODE(st.st_mode)
            assert mode == 0o600, f"Expected socket mode 0o600, got {oct(mode)}"

        # Socket must be cleaned up on server exit
        assert not os.path.exists(sock_path)

    def test_server_socket_permissions_bits_breakdown(self, tmp_path: pathlib.Path) -> None:
        """Verify owner read/write and zero group/world permission bits."""
        sock_path = str(tmp_path / "vmafx-sidecar.sock")
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            mode = stat.S_IMODE(st.st_mode)

            # User permissions: must have read and write (0o600), no execute
            assert (
                mode & 0o700
            ) == 0o600, f"User bits must be rw- (0o600), got {oct(mode & 0o700)}"

            assert (mode & 0o070) == 0, f"Group bits must be --- (0o000), got {oct(mode)}"

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

    def test_socket_recreation_cleans_stale_socket_with_wrong_permissions(
        self, tmp_path: pathlib.Path
    ) -> None:
        """A pre-existing stale socket with different permissions must be cleanly replaced."""
        sock_path = str(tmp_path / "stale.sock")
        # Pre-create a stale socket file with overly broad permissions.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale_srv:
            stale_srv.bind(sock_path)
            os.chmod(sock_path, 0o666)

        assert os.path.exists(sock_path)
        assert stat.S_IMODE(os.stat(sock_path).st_mode) == 0o666

        stale_identity = os.lstat(sock_path)
        with self._running_server(sock_path):
            st = os.stat(sock_path)
            assert stat.S_IMODE(st.st_mode) == 0o600
            assert (st.st_dev, st.st_ino) != (stale_identity.st_dev, stale_identity.st_ino)

    def test_server_refuses_regular_file_without_removing_it(self, tmp_path: pathlib.Path) -> None:
        """A configured socket path must never authorize deletion of an ordinary file."""
        socket_path = tmp_path / "ordinary-file.sock"
        socket_path.write_text("keep me", encoding="utf-8")
        stopped = threading.Event()
        stopped.set()

        with pytest.raises(FileExistsError) as exc_info:
            ot_mod.run_server(
                socket_path=str(socket_path),
                trainer=mock.MagicMock(),
                stop_event=stopped,
            )

        assert exc_info.value.errno == errno.EEXIST
        assert socket_path.read_text(encoding="utf-8") == "keep me"

    def test_second_server_refuses_live_socket_without_disrupting_owner(
        self, tmp_path: pathlib.Path
    ) -> None:
        """A contender must not unlink or replace a live server endpoint."""
        sock_path = str(tmp_path / "live.sock")
        with self._running_server(sock_path):
            owner = os.lstat(sock_path)
            already_stopped = threading.Event()
            already_stopped.set()

            with pytest.raises(OSError) as exc_info:
                ot_mod.run_server(
                    socket_path=sock_path,
                    trainer=mock.MagicMock(),
                    stop_event=already_stopped,
                )

            assert exc_info.value.errno == errno.EADDRINUSE
            current = os.lstat(sock_path)
            assert (current.st_dev, current.st_ino) == (owner.st_dev, owner.st_ino)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.connect(sock_path)

    def test_second_server_refuses_endpoint_during_bind_listen_window(
        self, tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A bound-but-not-listening owner must not be mistaken for a stale socket."""
        socket_path = str(tmp_path / "starting.sock")
        owner_bound = threading.Event()
        allow_listen = threading.Event()

        class PausedFirstListener(socket.socket):
            def listen(self, backlog: int = 0) -> None:
                if self.getsockname() == socket_path and not owner_bound.is_set():
                    owner_bound.set()
                    assert allow_listen.wait(timeout=3.0)
                super().listen(backlog)

        monkeypatch.setattr("ai.sidecar.online_trainer.socket.socket", PausedFirstListener)
        owner_stop = threading.Event()
        owner_errors: list[BaseException] = []

        def run_owner() -> None:
            try:
                ot_mod.run_server(
                    socket_path=socket_path,
                    trainer=mock.MagicMock(),
                    stop_event=owner_stop,
                )
            except BaseException as exc:  # surfaced in the parent assertion below
                owner_errors.append(exc)

        owner_thread = threading.Thread(target=run_owner, daemon=True)
        owner_thread.start()
        assert owner_bound.wait(timeout=3.0)
        owner = os.lstat(socket_path)

        contender_stop = threading.Event()
        contender_stop.set()
        try:
            with pytest.raises(OSError) as exc_info:
                ot_mod.run_server(
                    socket_path=socket_path,
                    trainer=mock.MagicMock(),
                    stop_event=contender_stop,
                )
            assert exc_info.value.errno == errno.EADDRINUSE
            current = os.lstat(socket_path)
            assert (current.st_dev, current.st_ino) == (owner.st_dev, owner.st_ino)
        finally:
            allow_listen.set()
            owner_stop.set()
            owner_thread.join(timeout=3.0)

        assert not owner_thread.is_alive()
        assert owner_errors == []

    def test_server_refuses_symlink_without_touching_link_or_target(
        self, tmp_path: pathlib.Path
    ) -> None:
        """Startup must inspect the pathname itself and never follow or remove a symlink."""
        target = tmp_path / "target.txt"
        target.write_text("sentinel", encoding="utf-8")
        socket_path = tmp_path / "sidecar.sock"
        socket_path.symlink_to(target)

        stopped = threading.Event()
        stopped.set()
        with pytest.raises(FileExistsError) as exc_info:
            ot_mod.run_server(
                socket_path=str(socket_path),
                trainer=mock.MagicMock(),
                stop_event=stopped,
            )

        assert exc_info.value.errno == errno.EEXIST
        assert socket_path.is_symlink()
        assert socket_path.readlink() == target
        assert target.read_text(encoding="utf-8") == "sentinel"

    def test_shutdown_preserves_socket_rebound_at_owned_path(self, tmp_path: pathlib.Path) -> None:
        """Cleanup must not unlink a replacement socket published after startup."""
        socket_path = str(tmp_path / "rebound.sock")
        stop_event = threading.Event()
        server_thread = threading.Thread(
            target=ot_mod.run_server,
            kwargs={
                "socket_path": socket_path,
                "trainer": mock.MagicMock(),
                "stop_event": stop_event,
            },
            daemon=True,
        )
        server_thread.start()

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                owner = os.lstat(socket_path)
                if stat.S_ISSOCK(owner.st_mode):
                    break
            except FileNotFoundError:
                pass
            time.sleep(0.01)
        else:
            pytest.fail("original server did not publish its socket")

        os.unlink(socket_path)
        replacement = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            replacement.bind(socket_path)
            replacement.listen(1)
            rebound = os.lstat(socket_path)
            assert (rebound.st_dev, rebound.st_ino) != (owner.st_dev, owner.st_ino)

            stop_event.set()
            server_thread.join(timeout=2.0)
            assert not server_thread.is_alive()

            current = os.lstat(socket_path)
            assert (current.st_dev, current.st_ino) == (rebound.st_dev, rebound.st_ino)
        finally:
            stop_event.set()
            server_thread.join(timeout=2.0)
            replacement.close()
            with contextlib.suppress(FileNotFoundError):
                os.unlink(socket_path)

    def test_shutdown_preserves_regular_file_replacing_owned_socket(
        self, tmp_path: pathlib.Path
    ) -> None:
        """Cleanup must not remove a non-socket replacement at the configured path."""
        socket_path = str(tmp_path / "replacement-file.sock")
        stop_event = threading.Event()
        server_thread = threading.Thread(
            target=ot_mod.run_server,
            kwargs={
                "socket_path": socket_path,
                "trainer": mock.MagicMock(),
                "stop_event": stop_event,
            },
            daemon=True,
        )
        server_thread.start()

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            with contextlib.suppress(FileNotFoundError):
                if stat.S_ISSOCK(os.lstat(socket_path).st_mode):
                    break
            time.sleep(0.01)
        else:
            pytest.fail("server did not publish its socket")

        os.unlink(socket_path)
        pathlib.Path(socket_path).write_text("replacement", encoding="utf-8")
        stop_event.set()
        server_thread.join(timeout=2.0)

        assert not server_thread.is_alive()
        assert pathlib.Path(socket_path).read_text(encoding="utf-8") == "replacement"

    def test_owner_ipc_connection_succeeds_under_0o600(self, tmp_path: pathlib.Path) -> None:
        """The same-UID client exchanges messages over the owner-only endpoint."""
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
        socket is already registered atomically in _ConnectionRegistry by run_server, ensuring
        deterministic and prompt shutdown (< 1.5s) without thread join timeouts.
        """
        sock_path = str(tmp_path / "race_test.sock")
        dummy_trainer = mock.MagicMock()
        stop_event = threading.Event()

        orig_handle = ot_mod._handle_connection

        def delayed_handle(*args: Any, **kwargs: Any) -> Any:
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

    def test_owner_only_server_rejects_different_uid_same_gid_peer(
        self, shared_ipc_dir: pathlib.Path
    ) -> None:
        """The shipped endpoint does not grant access based on group membership."""
        sock_path = str(shared_ipc_dir / "different_uid.sock")
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
        with self._running_server(sock_path):
            peer = _spawn_peer(child_code)
            output = _finish_peer(peer)

        assert peer.uid != os.geteuid()
        assert peer.gid == os.getegid()
        assert output.strip() == "expected-eacces"


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
