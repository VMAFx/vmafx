- **Semgrep warning alerts 946–949 audit and remediation (Research-2095)** — upgraded
  memoization keys to pure SHA-256, hardened concurrency and server lifecycle, and audited socket permissions:
  (1) `compat/python-vmaf/tools/decorator.py` (`@persist`, `@persist_to_file`, `@persist_to_dir`)
  upgraded from SHA-1 to `hashlib.sha256(..., usedforsecurity=False)` under ADR-1307 (partially superseding
  ADR-1222 for its SHA-1 keep-open disposition); clean cold invalidation of legacy
  caches accepted for ephemeral memoization; cross-process concurrency serialized via re-entrant
  platform-native file locks (`fcntl.flock` on POSIX and `msvcrt.locking` on Windows,
  guarded by in-process `threading.RLock()`), disk cache reloads and merges under lock on cache miss,
  and atomic writes via `tempfile.mkstemp` and `os.replace`; eliminating
  SHA-1 entirely yields 0 SARIF findings and genuinely resolves Semgrep alerts 947, 948, and 949 at source;
  (2) `ai/sidecar/online_trainer.py` audited for Semgrep alert 946 (mode `0o660`): verified
  that `0o660` is an intentional, least-privilege Unix domain socket permission
  required for same-group Go peer IPC in Kubernetes pod emptyDir mounts (connecting
  requires `stat.S_IWGRP`, whereas Semgrep's suggested `0o644` breaks client connection
  and leaks world read); replaced the `active_conns` + `conns_lock` clump with a private atomic
  `_ConnectionRegistry` abstraction (`register_if_active`, `discard`, `close_all`); hardened server lifecycle with pre-bind
  variable initialization (preventing `UnboundLocalError` on bind failure), atomic socket tracking registration
  before worker thread spawn eliminating accept-vs-shutdown races, bounded thread joins, and prompt
  active connection shutdown unblocking held clients on server stop (< 1.5s shutdown); retained
  inline `# nosemgrep` directive; alert remains open in SARIF/GitHub Code Scanning awaiting maintainer
  dismissal per ADR-1222;
  (3) added red-capable regression suites in `test_decorator_extended.py` (26 tests passing: multiprocess concurrent
  updates without clobber, cross-process cache hits, reentrant recursion, golden vectors, cold
  invalidation) and `test_socket_permissions.py` (16 tests passing: accept-vs-shutdown registration race, accept after
  stop, mode 0o660, ownership, bind failure, held client shutdown, connection-registry unit tests, and real
  different-EUID/same-GID client simulation under Linux user namespaces asserting `SO_PEERCRED`); wired sidecar
  test collection across root `pyproject.toml`, `ai/pyproject.toml`, `noxfile.py`, and CI.
