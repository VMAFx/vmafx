<!-- markdownlint-disable MD013 MD060 -->
# Research-2078: Semgrep Warning Alerts 946–949 Audit and Resolution

- **Status**: Closed (implemented in branch `fix/semgrep-python-warning-alerts`)
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Alerts Triaged**: GitHub Code Scanning Alerts 946, 947, 948, 949

## Executive Summary

This research document records the security audit, architectural analysis, and resolution for four open Semgrep warning-level code scanning alerts on fresh `master`:

1. **Alerts 947, 948, 949**: `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` at [`compat/python-vmaf/tools/decorator.py:48, 120, 147`](../../compat/python-vmaf/tools/decorator.py).
   - **Resolution**: Upgraded memoization keys from SHA-1 to SHA-256 (`hashlib.sha256(..., usedforsecurity=False)`). Implemented **backward-compatible read-through migration**: new cache keys and files strictly use SHA-256; if the primary SHA-256 key/file is missing, the legacy SHA-1 key is checked only for compatibility lookup. Existing legacy values are loaded and atomically promoted to the SHA-256 key/file via `_write_json_cache_atomic`. New entries never use SHA-1. Red-capable unit tests prove 64-hex digest formatting, golden vectors, function-name differentiation, argument permutation collision resistance, and explicit preloaded legacy SHA-1 file/directory read-through migration without re-computation. Resolves Semgrep alerts 947, 948, 949 with documented compatibility-only exception comments.
2. **Alert 946**: `python.lang.security.audit.insecure-file-permissions.insecure-file-permissions` at [`ai/sidecar/online_trainer.py:430`](../../ai/sidecar/online_trainer.py).
   - **Resolution**: Audited and confirmed that permission mode `0o660` (`rw-rw----`) is an **intentional, required group-shared IPC artifact**, not an over-permissive vulnerability. Semgrep's suggested fix (`0o644`) strips group write and immediately breaks the Go client's ability to connect and transmit online feedback samples in Kubernetes pod deployments. Mode `0o660` enforces minimal privilege: user read/write, group read/write, and strictly zero world permissions (`0o000`). Red-capable regression tests cover mode bits, ownership, stale socket recreation, and IPC roundtrip. An inline documented scanner directive is maintained per [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md), and a formal maintainer dismissal recommendation ("Won't Fix" / "False Positive") is recorded.

---

## Part 1: Alerts 947–949 (SHA-1 in `compat/python-vmaf/tools/decorator.py`)

### 1.1 Alert Details

- **Alert IDs**: 947, 948, 949
- **Rule ID**: `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1`
- **Locations**:
  - `compat/python-vmaf/tools/decorator.py:48` (in function `persist`)
  - `compat/python-vmaf/tools/decorator.py:120` (in function `persist_to_file`)
  - `compat/python-vmaf/tools/decorator.py:147` (in function `persist_to_dir`)
- **Scanner Message**: "Detected SHA1 hash algorithm which is considered insecure. SHA1 is not collision resistant and is therefore not suitable as a cryptographic signature. Consider using a stronger alternative such as SHA256."

### 1.2 Root Cause and Cache-Key Analysis

In `decorator.py`, decorators `@persist`, `@persist_to_file`, and `@persist_to_dir` serialize function arguments using `(str(func.__name__) + str(args)).encode()` and compute a hash digest to form cache keys.

An essential question was whether migrating from SHA-1 to SHA-256 would invalidate durable on-disk cache files across user installations:

1. **Scope of `decorator.py`**: A complete codebase search reveals that `decorator.py` is an upstream Netflix utility module used for in-memory and ephemeral directory function memoization.
2. **Durable Cache Decoupling**: Durable pipeline artifacts in VMAFx (such as `.vmaf` output stores, `Result` serialization in `compat/python-vmaf/core/result.py`, and `Asset` hashing in `compat/python-vmaf/core/asset.py` / `executor.py`) do **not** use `decorator.py`. Those components maintain their own hashing mechanisms.
3. **No Retraining or Golden Breakage**: Upgrading `decorator.py` to SHA-256 does not alter any Netflix golden assertions (which run against core scoring paths in `python/test/`) or any trained model checkpoints.
4. **Backward-Compatible Migration**: To avoid cold cache misses on existing user environments containing pre-existing `persist_to_file` JSON files or `persist_to_dir` caches, a read-through fallback mechanism was implemented:
   - Modern SHA-256 is checked first.
   - If missing, the legacy SHA-1 key is looked up.
   - If found, the entry is promoted atomically to SHA-256 so subsequent accesses hit the primary key directly.
   - New computations write strictly to SHA-256; new SHA-1 entries are never created.

### 1.3 Implementation and Verification

- In `compat/python-vmaf/tools/decorator.py`:
  - Added `_write_json_cache_atomic(file_path, data)` using PID-tagged temporary sibling files and `os.replace` for cross-platform atomicity.
  - Implemented read-through fallback in `persist`, `persist_to_file`, and `persist_to_dir`:
    1. Primary lookup uses `hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()`.
    2. Fallback lookup uses `hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()` annotated with `# nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` as a reviewed compatibility-only site.
    3. On hit, the legacy entry is promoted atomically to SHA-256.
    4. On miss, `original_func(*args)` is evaluated and stored strictly under SHA-256.
- Added comprehensive red-capable regression tests in `compat/vmaf/tests/test_decorator_extended.py`:
  - `test_cache_key_digest_is_sha256_length_and_hex`: asserts 64-character length and valid hexadecimal encoding.
  - `test_cache_dir_filename_is_sha256`: asserts disk cache filenames use the 64-hex SHA-256 format.
  - `test_cache_key_stability_golden_vectors`: validates exact golden vector digests against known inputs.
  - `test_cache_key_differentiates_distinct_functions_with_same_args`: proves distinct function names generate distinct cache entries.
  - `test_cache_key_differentiates_argument_permutations_and_types`: proves argument ordering and types generate distinct keys.
  - `test_persist_to_file_legacy_sha1_read_through_migration`: verifies preloaded legacy SHA-1 JSON cache entries hit without re-executing function and promote atomically to SHA-256.
  - `test_persist_to_dir_legacy_sha1_read_through_migration`: verifies pre-existing SHA-1 disk files hit without re-executing and promote atomically to SHA-256 files.
  - `test_persist_in_memory_legacy_migration`: verifies in-memory closure promotion from legacy SHA-1 to SHA-256.
  - `test_persist_in_memory_memoization_stability`: verifies `@persist` in-memory recursion memoization.
- Verified: `semgrep scan --config=p/python compat/python-vmaf/tools/decorator.py` reports **0 findings**. With `--disable-nosem`, only the 3 legacy compatibility sites are flagged.

---

## Part 2: Alert 946 (Mode `0o660` in `ai/sidecar/online_trainer.py`)

### 2.1 Alert Details

- **Alert ID**: 946
- **Rule ID**: `python.lang.security.audit.insecure-file-permissions.insecure-file-permissions`
- **Location**: `ai/sidecar/online_trainer.py:430`
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

### 2.3 Implementation & Regression Guards

- Retained mode `0o660` in `ai/sidecar/online_trainer.py:430` with documented rationale and inline `# nosemgrep` directive per [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md).
- Hardened `run_server()` lifecycle:
  - Added optional `stop_event: threading.Event | None = None` parameter for deterministic test and worker lifecycle control.
  - Wrapped server socket operations in `try ... finally: srv.close(); os.unlink(socket_path)` to ensure deterministic socket cleanup.
  - Wrapped signal registration in `with contextlib.suppress(ValueError):` so `run_server()` can be invoked safely in test threads.
  - Added connection thread joining on shutdown to guarantee zero leaked socket descriptors.
- Created `ai/sidecar/tests/test_socket_permissions.py` with 6 red-capable regression tests:
  - `test_server_socket_mode_exact_0o660`: validates exact mode `0o660`.
  - `test_server_socket_permissions_bits_breakdown`: validates user `rw-`, group `rw-`, and zero other/world bits.
  - `test_server_socket_ownership`: validates UID/GID matching EUID/EGID.
  - `test_red_capable_mode_regression_guards`: proves mathematical rejection of `0o644`, `0o600`, and `0o666`.
  - `test_socket_recreation_cleans_stale_socket_with_wrong_permissions`: tests unlinking stale socket files with wrong permissions.
  - `test_ipc_connection_succeeds_under_0o660`: tests end-to-end client JSON connection, ingestion, and ACK receipt over `0o660`.

### 2.4 Maintainer-Dismissal Recommendation

Per [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md), agent dismissals via GitHub API are prohibited; alert dismissal is reserved for repository maintainers.

- **Alert ID**: 946
- **Recommended Status**: Dismissed as **False Positive** or **Won't Fix**
- **Justification Note**: Mode `0o660` is required for Unix domain socket stream IPC with same-group peer containers in Kubernetes pods (connecting requires group write `stat.S_IWGRP`). World permissions are strictly zero (`0o000`). Covered by unit tests in `ai/sidecar/tests/test_socket_permissions.py` and documented in Research-2078.

---

## Part 3: Test and Tooling Verification Summary

| Target / Check | Tool | Result |
|---|---|---|
| `compat/vmaf/tests/test_decorator_extended.py` | `pytest` | 22 passed (0.05s) |
| `ai/sidecar/tests/` | `pytest` | 21 passed, 2 skipped (0.08s) |
| `compat/python-vmaf/tools/decorator.py` | `semgrep (p/python)` | 0 findings (3 ignored via inline `# nosemgrep` for legacy lookup) |
| `ai/sidecar/online_trainer.py` | `semgrep (p/python)` | 0 findings (1 ignored via inline `# nosemgrep` for group IPC) |
| Scanned files with `--disable-nosem` | `semgrep (p/python)` | 4 findings (the exact 4 audited sites) |
| Whole repository | `semgrep (.semgrep.yml)` | 0 findings (1040 files) |
| Code formatting | `black --check` | Clean (0 files to reformat) |
| Code linting | `ruff check` | All checks passed (Clean) |
| State integrity | `check-state-md-rows.sh` | OK (no duplicate IDs, matching status) |
