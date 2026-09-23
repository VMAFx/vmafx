<!-- markdownlint-disable MD013 MD031 MD032 MD060 -->
# Research-2095: Semgrep Warning Alerts 946–949 Audit and Resolution

- **Status**: Closed (implemented in branch `fix/semgrep-python-warning-alerts`)
- **Date**: 2026-09-23 (updated 2026-09-24)
- **Deciders**: Lusoris
- **Alerts Triaged**: GitHub Code Scanning Alerts 946, 947, 948, 949

## Executive Summary

This research document records the security audit, architectural analysis, and resolution for four open Semgrep warning-level code scanning alerts on fresh `master`:

1. **Alerts 947, 948, 949**: `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` at [`compat/python-vmaf/tools/decorator.py`](../../compat/python-vmaf/tools/decorator.py).
   - **Resolution**: Upgraded memoization keys from SHA-1 to pure SHA-256 (`hashlib.sha256(..., usedforsecurity=False)`). Governed by [ADR-1307](../adr/1307-sha256-memoization-cache-invalidation.md), which explicitly supersedes [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md) only for its SHA-1 keep-open disposition. Audited the compatibility contract: `decorator.py` is strictly used for in-memory and ephemeral directory function memoization, completely decoupled from durable pipeline outputs, model assets, and Netflix golden assertions. In-memory `persist` caches cannot survive process restarts; accepting clean cold invalidation for file/directory memoization allowed complete removal of legacy SHA-1 fallback and associated `# nosemgrep` comments. Consequently, Semgrep default `p/python` produces **0 findings in SARIF** for `compat/python-vmaf/tools/decorator.py`, genuinely closing GitHub alerts 947, 948, and 949 upon upload without scanner disguise or SARIF filtering.
   - **Concurrency & Cross-Process Hardening**: `_write_json_cache_atomic` replaced PID-based temp files (`target.tmp.pid`) with secure unique temporary file creation via `tempfile.mkstemp(dir=file_dir, prefix=f".{base_name}.", suffix=".tmp")` with error cleanup (`os.close` and `os.unlink`). Cross-process file locking via re-entrant `_file_lock` (`fcntl.flock` on POSIX and `msvcrt.locking` on Windows) and disk cache reloading/merging guarantees that concurrent processes never clobber each other's cache entries, while same-process concurrent read/modify/write is serialized using `threading.RLock()`, avoiding recursion deadlock on dynamic programming memoization.
2. **Alert 946**: `python.lang.security.audit.insecure-file-permissions.insecure-file-permissions` on the `os.chmod(socket_path, 0o660)` call in [`ai/sidecar/online_trainer.py`](../../ai/sidecar/online_trainer.py).
   - **Resolution**: Audited and confirmed that permission mode `0o660` (`rw-rw----`) is an **intentional, required group-shared IPC artifact**, not an over-permissive defect. Semgrep's suggested fix (`0o644`) strips group write (`stat.S_IWGRP`) and immediately breaks the Go client's ability to connect and transmit online feedback samples in Kubernetes pod deployments. Mode `0o660` enforces minimal privilege: user read/write, group read/write, and strictly zero world permissions (`0o000`).
   - **Connection Registry & Lifecycle Hardening**: Replaced the previous `active_conns` + `conns_lock` parameter clump with a private `_ConnectionRegistry` abstraction providing synchronized `register_if_active()`, `discard()`, and `close_all()` operations, eliminating half-states and races without broad redesign. Pre-bind initialization of server lifecycle structures (`threads`, `registry`, `old_sigterm`, `old_sigint`) prevents bind failure exceptions from being masked by `UnboundLocalError`. Accepted sockets are atomically registered via `registry.register_if_active(conn, stop_event)` prior to worker thread spawn, ensuring any connection accepted immediately before or during shutdown is promptly shut down (`socket.SHUT_RDWR`) and closed, unblocking all worker threads and guaranteeing deterministic shutdown (< 1.5s). Signal handlers are safely restored on exit.
   - **Real Cross-UID / Same-GID Peer Verification**: Verified via Linux user namespaces with subordinate ID mappings (`newuidmap` / `newgidmap`) or root credential drop, asserting actual peer credentials (`SO_PEERCRED`), proving a real different-EUID, same-GID client connects and exchanges IPC under mode `0o660`, while failing with `PermissionError` (`EACCES`) under modes `0o644` and `0o600`. Tests explicitly skip on non-POSIX hosts and when a POSIX host cannot provide root credential drop or subordinate Linux user namespaces.
   - **SARIF & GitHub Alert Status**: An inline documented `# nosemgrep` directive is retained per [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md). In Semgrep's SARIF output, in-source suppressions are reported with `"suppressions": [{"kind": "inSource"}]`. Because `security-scans.yml` uploads raw SARIF to GitHub Code Scanning, GitHub maintains in-source suppressed alerts as open. Therefore, Alert 946 cannot be claimed as closed by code changes alone; it remains open in SARIF awaiting formal repository maintainer dismissal ("Won't Fix" / "False Positive").

---

## Part 1: Alerts 947–949 (SHA-1 in `compat/python-vmaf/tools/decorator.py`)

### 1.1 Alert Details

- **Alert IDs**: 947, 948, 949
- **Rule ID**: `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1`
- **Locations**:
  - `compat/python-vmaf/tools/decorator.py` (in `@persist`, `@persist_to_file`, and `@persist_to_dir`)
- **Scanner Message**: "Detected SHA1 hash algorithm which is considered insecure. SHA1 is not collision resistant and is therefore not suitable as a cryptographic signature. Consider using a stronger alternative such as SHA256."

### 1.2 Architectural & Compatibility Analysis: Clean SHA-256 vs Read-Through Fallback

In `decorator.py`, decorators `@persist`, `@persist_to_file`, and `@persist_to_dir` serialize function arguments using `(str(func.__name__) + str(args)).encode()` and compute a hash digest to form cache keys.

During initial triage, a read-through fallback mechanism using `# nosemgrep` suppressions was considered to preserve pre-existing legacy SHA-1 cache entries. However, detailed investigation in [ADR-1307](../adr/1307-sha256-memoization-cache-invalidation.md) revealed decisive facts that led to explicitly superseding [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md) for its SHA-1 keep-open disposition:

1. **ADR-1222 Supersession and SARIF Behavior**: In Semgrep CLI, an inline `# nosemgrep` comment suppresses terminal console output, but default SARIF generation (`semgrep scan --config=p/python --sarif`) still includes the findings with an annotated suppression property:

   ```json
   "suppressions": [{"kind": "inSource"}]
   ```

   When `security-scans.yml` uploads this SARIF to GitHub Code Scanning, GitHub does not close alerts that are present in SARIF. Thus, retaining legacy SHA-1 lookups with in-code suppression would leave alerts 947–949 open on GitHub, falsifying any claim of hosted alert resolution.
2. **Compatibility Contract of `decorator.py`**:
   - `decorator.py` is an upstream Netflix utility module used exclusively for runtime memoization.
   - In-memory `@persist` closures exist only for process lifetime and cannot survive restarts; SHA-1 fallback here was entirely redundant.
   - For `@persist_to_file` and `@persist_to_dir`, cache keys represent ephemeral, reconstructible intermediate function evaluations.
   - Durable pipeline artifacts in VMAFx (such as `.vmaf` output stores, `Result` serialization in `compat/python-vmaf/core/result.py`, and `Asset` hashing in `compat/python-vmaf/core/asset.py` / `executor.py`) do **not** use `decorator.py`.
   - Core scoring routines and Netflix golden assertions (in `python/test/`) are completely unaffected.
   - Cold invalidation of legacy memoization files on upgrade is entirely safe and normal for cache upgrades.
3. **Rejected Alternatives Evaluated in ADR-1307**:
   - *Rejected Alternative 1: Dual Hashing (Read-Through Fallback with `# nosemgrep`)*: Look up SHA-256 first, then SHA-1 fallback, write SHA-256. Rejected because it preserves active SHA-1 invocations that fire Semgrep in SARIF (`suppressions: [inSource]`), preventing hosted GitHub alerts 947–949 from closing, while adding dead branching complexity for ephemeral function caches.
   - *Rejected Alternative 2: External File-Locking Dependencies (`filelock`, `fasteners`)*: Adding external dependencies to `compat/python-vmaf` violates the zero-new-dependencies policy for upstream compatibility and adds packaging surface.
   - *Rejected Alternative 3: PID-Based Temp Files (`target.tmp.pid`)*: Collides across multiple worker threads sharing the same process PID, causing `FileNotFoundError` and JSON corruption under multi-threaded execution.

Therefore, per [ADR-1307](../adr/1307-sha256-memoization-cache-invalidation.md), the decision was made to remove SHA-1 entirely from `decorator.py`. All decorators compute strictly SHA-256 digests (`hashlib.sha256(..., usedforsecurity=False)`).

### 1.3 Concurrency, Cross-Process Safety & Atomic File Writing

Critical defects were identified in concurrent multi-threaded and cross-process usage of `persist_to_file` and `persist_to_dir`:
1. `_write_json_cache_atomic` previously used `f"{file_path}.tmp.{os.getpid()}"`. Because all threads in a process share the same PID, concurrent cache updates by multiple worker threads collided on the same temporary file, causing `FileNotFoundError` and corrupted JSON.
2. Unsynchronized read-modify-write cycles across concurrent worker threads and external processes in `persist_to_file` caused race conditions and silent cache clobbering, where one process completely overwrote keys computed and persisted by another process.
3. Standard `threading.Lock()` causes immediate thread deadlocks when `@persist` or `@persist_to_file` is used for recursive algorithms (e.g., dynamic programming memoization).

**Fixes Applied**:
- `_write_json_cache_atomic` creates unique temporary files in the target directory using `tempfile.mkstemp(dir=file_dir, prefix=f".{base_name}.", suffix=".tmp")` with error cleanup (`os.close` and `os.unlink`).
- `_file_lock` implements a re-entrant cross-process file lock (`fcntl.flock` on POSIX and `msvcrt.locking` on Windows, backed by a thread-safe `threading.RLock()` and thread-local re-entrancy tracker) over `f"{file_name}.lock"`.
- Python 3.14's [`msvcrt.locking`](https://docs.python.org/3/library/msvcrt.html#msvcrt.locking) contract locks from the current file position, permits a region beyond end-of-file, and bounds `LK_LOCK` acquisition to ten retries; the implementation seeks to byte zero before both lock and unlock.
- `persist_to_file` reloads and merges disk state under `_file_lock` on cache misses, then atomically writes the full merged in-memory cache so concurrent processes and recursive calls never overwrite each other's entries.
- `threading.RLock()` is instantiated per decorator instance, allowing re-entrant acquisition by the same thread during recursive calls while serializing concurrent threads.

### 1.4 Verification and SARIF Elimination

- **SARIF Verification**: Running `semgrep scan --config=p/python --sarif compat/python-vmaf/tools/decorator.py` outputs **0 results in SARIF** (`"results": []`). Alerts 947, 948, and 949 are 100% eliminated from the SARIF report without scanner disguise or post-scan filtering.
- **Unit Tests (`compat/vmaf/tests/test_decorator_extended.py`)**: 26 passed:
  - `test_cache_key_digest_is_sha256_length_and_hex`: validates 64-character hex length.
  - `test_cache_dir_filename_is_sha256`: validates 64-hex filename formatting.
  - `test_cache_key_stability_golden_vectors`: validates exact golden vector digests against known inputs.
  - `test_cache_key_differentiates_distinct_functions_with_same_args`: validates function name differentiation.
  - `test_cache_key_differentiates_argument_permutations_and_types`: validates argument ordering and typing.
  - `test_persist_to_file_concurrent_threads`: verifies 8 concurrent worker threads performing 50 parallel cache writes without errors or corrupt JSON.
  - `test_persist_to_dir_concurrent_threads`: verifies 8 concurrent worker threads writing parallel directory cache files.
  - `test_persist_to_file_cold_invalidation_behavior`: verifies that pre-existing legacy cache entries miss cleanly, evaluate the underlying function, and store the result under SHA-256.
  - `test_persist_in_memory_memoization_stability`: verifies re-entrant recursive memoization (Fibonacci) under `RLock`.
  - `test_persist_to_file_multiprocess_concurrent_updates_no_clobber`: verifies 8 concurrent processes updating `persist_to_file` on the same cache file without clobbering or losing keys.
  - `test_persist_to_file_cross_process_cache_hit`: verifies cross-process cache hit on disk avoids redundant function recomputation.
  - `test_persist_to_file_reentrant_recursion`: verifies recursive functions with `persist_to_file` re-enter file locks without self-deadlock.
  - `test_file_lock_uses_windows_byte_range_backend`: verifies the Windows fallback seeks to byte zero and dispatches matched `msvcrt` lock/unlock operations.

---

## Part 2: Alert 946 (Mode `0o660` in `ai/sidecar/online_trainer.py`)

### 2.1 Alert Details

- **Alert ID**: 946
- **Rule ID**: `python.lang.security.audit.insecure-file-permissions.insecure-file-permissions`
- **Location**: the `os.chmod(socket_path, 0o660)` call in `ai/sidecar/online_trainer.py`
- **Scanner Message**: "Detected file execution with insecure permissions. Files should generally be given read and write permissions only to the owner, unless wider permissions are required."

### 2.2 Architectural & Security Analysis: Why Mode `0o660` is Mandatory

In `ai/sidecar/online_trainer.py`, `run_server()` binds a Unix domain stream socket (`socket.AF_UNIX`, `socket.SOCK_STREAM`) at `socket_path` (default `/tmp/vmafx-sidecar.sock`).

Per [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md):

1. **Multi-Container Pod IPC**: In production Kubernetes deployments, the Python sidecar server (`ai/sidecar/online_trainer.py`) and the Go worker node (`cmd/vmafx-node/online_feedback.go`) run as separate containers within the same Pod, sharing an `emptyDir` volume.
2. **Linux Socket Permission Semantics**: On Linux, connecting to a Unix domain stream socket (`connect(2)`) requires **write permission** (`stat.S_IWGRP = 0o020`) on the socket special file.
3. **Flaw of Semgrep's Default Rule (`0o644`)**:
   - `0o644` sets user `rw-`, group `r--`, world `r--`.
   - Lacks group write (`S_IWGRP`). When the Go client runs as a different UID in the shared pod group (GID), connecting fails with `EACCES (Permission denied)`.
   - Furthermore, `0o644` grants world read (`S_IROTH = 0o004`), needlessly leaking socket visibility to unrelated system users.
4. **Flaw of User-Only Mode (`0o600`)**:
   - `0o600` grants permissions strictly to EUID.
   - Denies all group access, completely preventing multi-container pod architectures from sharing the socket across different non-root service accounts.
5. **Why `0o660` is Least-Privilege**:
   - User permissions: `rw-` (`0o600`) — owner has full read/write access.
   - Group permissions: `rw-` (`0o060`) — peers in the same group have read/write access to communicate over the IPC socket.
   - World permissions: `---` (`0o000`) — zero access granted to world/other users.
   - Execute bits: `---` (`0o000`) — no execution bits set.

### 2.3 Server Lifecycle Hardening and Connection Registry

Three lifecycle defects were identified and resolved in `online_trainer.py`, culminating in a private `_ConnectionRegistry` abstraction:
1. **Unmasked Bind Failures**: Previously, `threads` was declared after `srv.bind(...)`. When `bind()` failed (e.g., non-existent directory), the `finally:` block attempted to evaluate `for t in threads:`, raising an unhandled `UnboundLocalError` that masked the original `OSError`. Moving `threads: list[threading.Thread] = []`, `registry = _ConnectionRegistry()`, `old_sigterm`, and `old_sigint` before the `try:` block ensures that bind failures surface cleanly.
2. **Held Client Shutdown Hang**: Client connections blocked on `conn.recv()` survived server stop events, causing sequential `t.join(timeout=1.0)` calls that scaled shutdown delay by client count. Active connections are managed by `_ConnectionRegistry`. During server shutdown in `finally:`, `registry.close_all()` atomically drains the tracked set, then shuts down (`socket.SHUT_RDWR`) and closes each socket without holding the registry lock, unblocking all worker threads and ensuring deterministic shutdown in < 1.5s regardless of client count. Thread joins in `finally:` are bounded by a fixed deadline (`remaining = max(0.0, deadline - time.monotonic())`).
3. **Accept, Registration, and Shutdown Race**: If a client connected right as shutdown was signalled, delegating socket registration to the spawned worker thread created a race where `run_server` could execute its `finally:` connection cleanup before the worker thread registered the socket, causing `finally:` to miss the socket and wait out the full join timeout. The `_ConnectionRegistry.register_if_active(conn, stop_event)` method resolves this: if `stop_event` is already set, the newly accepted connection is closed immediately under lock without registering or spawning a thread; otherwise it is atomically registered under lock *before* spawning the worker thread. Previous signal handlers are safely restored on server exit.

### 2.4 Maintainer-Dismissal Recommendation and SARIF Reality

Per [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md):
- Retaining mode `0o660` requires an inline `# nosemgrep` comment to suppress terminal noise.
- However, Semgrep includes suppressed findings in SARIF output with `"suppressions": [{"kind": "inSource"}]`.
- GitHub Code Scanning keeps alerts open when they appear in SARIF.
- **Therefore, Alert 946 is NOT closed by code changes and must not be claimed as resolved on GitHub.**
- Alert 946 remains open in SARIF and GitHub Code Scanning awaiting formal maintainer dismissal.

**Dismissal Recommendation**:
- **Alert ID**: 946
- **Recommended Status**: Dismissed as **False Positive** or **Won't Fix**
- **Justification Note**: Mode `0o660` is required for Unix domain socket stream IPC with same-group peer containers in Kubernetes pods (connecting requires group write `stat.S_IWGRP`). World permissions are strictly zero (`0o000`). Covered by unit tests in `ai/sidecar/tests/test_socket_permissions.py` and documented in Research-2095.

### 2.5 Real Cross-UID/Same-GID Peer Verification and Runner Wiring

1. **Linux User Namespace Simulation**: `test_socket_permissions.py` uses `_peer_command` and `_spawn_peer` to run a real child client under a different effective UID and the server's effective GID. On Linux it maps the current UID plus the first assigned subordinate UID/GID range through `unshare`, `newuidmap`, and `newgidmap`; a root-runner fallback drops credentials directly. The test asserts actual peer credentials via `SO_PEERCRED` (`peer_uid == target_uid`, `peer_gid == target_gid`, `peer_uid != os.geteuid()`, `peer_gid == os.getegid()`), proves full IPC exchange under mode `0o660`, and verifies that modes `0o644` and `0o600` fail with `PermissionError` (`EACCES`). Tests skip with explicit reasons on non-POSIX hosts or where neither supported credential mechanism is available.
2. **Runner Collection Wiring**: `ai/sidecar/tests` is wired into:
   - Root `pyproject.toml`: added `"ai/sidecar/tests"` to `testpaths` and declared the existing `main` marker applied by `python/test/conftest.py`, preventing strict-marker collection from aborting before the sidecar suite runs.
   - `ai/pyproject.toml`: added `"sidecar/tests"` to `testpaths` and the repository parent to `pythonpath`, so package-local collection can import `ai.sidecar`.
   - `noxfile.py`: added `"ai/sidecar/tests/"` to the `ai` session.
   - `.github/workflows/tests-and-quality-gates.yml`: line 465 runs both `ai/tests/` and `ai/sidecar/tests/`.

---

## Part 3: Test and Tooling Verification Summary

| Target / Check | Tool | Result |
|---|---|---|
| `compat/vmaf/tests/test_decorator_extended.py` | `pytest` | 26 passed |
| `ai/sidecar/tests/` | `pytest` | 31 passed, 2 dependency skips |
| `ai/sidecar/tests/test_socket_permissions.py` | `pytest` | 16 passed |
| `compat/python-vmaf/tools/decorator.py` | `semgrep (p/python)` | 0 findings in text and SARIF (Alerts 947–949 eliminated) |
| `ai/sidecar/online_trainer.py` | `semgrep (p/python)` | 0 text findings; exactly 1 in SARIF (Alert 946 in-source suppressed) |
| Scanned files combined | `semgrep (p/python)` SARIF | Exactly 1 result (Alert 946 suppressed, 0 for 947–949) |
| Whole repository | `semgrep (.semgrep.yml)` | 0 findings (1040 files) |
| Code formatting | `black --check` | Clean |
| Code linting | `ruff check` | Clean |
| State integrity | `check-state-md-rows.sh` | OK |
| ADR links | `check-adr-links.py` | OK |
