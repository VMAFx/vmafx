<!-- markdownlint-disable MD013 MD031 MD032 MD060 -->
# Research-2095: Semgrep Warning Alerts 946–949 Audit and Resolution

- **Status**: Closed (implemented in branch `fix/semgrep-python-warning-alerts`)
- **Date**: 2026-09-23 (corrected 2026-09-24)
- **Deciders**: Lusoris
- **Alerts Triaged**: GitHub Code Scanning Alerts 946, 947, 948, 949

## Executive Summary

All four Python Semgrep findings are removed at source.

1. **Alerts 947–949 — SHA-1 memoization keys**: the three decorators in
   [`compat/python-vmaf/tools/decorator.py`](../../compat/python-vmaf/tools/decorator.py)
   now use only `hashlib.sha256(..., usedforsecurity=False)`. Their caches are
   reconstructible, so legacy SHA-1 entries cold-miss without a compatibility
   fallback. [ADR-1307](../adr/1307-sha256-memoization-cache-invalidation.md)
   supersedes ADR-1222 for these alerts.
2. **Alert 946 — group-writable Unix socket**: the earlier claim that production
   required cross-UID/same-GID access was false. The current Helm chart does not
   wire the sidecar helper, rejects `sidecar.*` values, and assigns a single
   pod UID/GID. The unauthenticated endpoint now uses `0o600`; no suppression
   remains. [ADR-1309](../adr/1309-socket-path-ownership-and-owner-only-mode.md)
   supersedes ADR-1222 for this alert.
3. **Lifecycle audit**: `run_server()` no longer blindly unlinks its configured
   path. A lifetime claim lock, no-follow type checks, active/stale probing,
   device/inode validation, and identity-checked cleanup prevent cooperating
   servers and replacement paths from being deleted.
4. **Required-suite correction**: wiring `ai/sidecar/tests` into the required AI
   lane exposed a batch-size-one MSE broadcast warning and the legacy ONNX
   `dynamic_axes` warning under PyTorch 2.14. Predictions and targets are now
   equal-length vectors with an explicit mismatch error, and the opset-17
   exporter uses tuple arguments plus `dynamic_shapes`. The complete suite is
   warning-clean rather than merely its alert-focused subset.

The previous version of this digest incorrectly said the base cache writer used
PID-suffixed temporary files. Exact-base inspection shows it wrote JSON directly
to the destination with `open(file_name, "wt")`. Unique `mkstemp` files are new
atomic-write hardening, not a replacement for a PID-temp implementation.

## Part 1: Alerts 947–949 — pure SHA-256 memoization

### Cache contract

`@persist`, `@persist_to_file`, and `@persist_to_dir` hash the wrapped function
name plus its arguments to identify cached evaluations. The values are runtime
memoization only:

- in-memory entries cannot survive a process restart;
- file and directory entries are reconstructible intermediate evaluations;
- durable VMAFX results, models, and Netflix golden assertions do not consume
  these cache keys.

Clean invalidation is therefore the honest compatibility policy. A dual SHA-1
read path would retain the scanner finding and add permanent migration logic for
data that can simply be recomputed.

### Concurrency and atomicity audit

The exact base implementation loaded a JSON dictionary and then wrote the full
dictionary directly to the destination. It had no inter-process coordination
and no atomic replacement. The corrected implementation adds:

- a per-decorator `threading.RLock` for thread serialization and recursive
  memoization;
- a re-entrant lock file using `fcntl.flock` on POSIX and `msvcrt.locking` on
  Windows;
- reload-and-merge under the cross-process lock before computing/writing a miss;
- a unique same-directory file from `tempfile.mkstemp`, followed by
  `os.replace`, with descriptor and temporary-file cleanup on every exception.

PID-suffixed temporary files were considered and rejected because threads share
a PID. They never existed in the reviewed base.

### Hosted regression coverage

The 26 tests in `compat/vmaf/tests/test_decorator_extended.py` cover exact
SHA-256 vectors, clean cold invalidation, recursive re-entry, thread contention,
spawn-process cache merging, cross-process hits, and the Windows byte-range lock
backend. They run through the `compat_decorator` Nox session and the hosted
`build.yml` matrix. The Windows matrix repairs a checkout that materialized the
tracked `compat/vmaf` link as text by creating a directory junction, so the
spawn tests execute against the real `msvcrt` backend rather than a Linux mock.

## Part 2: Alert 946 — owner-only endpoint and safe pathname lifecycle

### Deployment evidence

The original `0o660` rationale cited two containers under different UIDs in one
production pod. Repository evidence contradicts that statement:

- `deploy/helm/vmafx/templates/sidecar-trainer.yaml` defines a helper and names
  removed `node-deployment.yaml` as its consumer;
- the current `templates/node.yaml` never includes that helper;
- `values.schema.json` has `additionalProperties: false` and no top-level
  `sidecar` property, so the documented enablement value is rejected;
- `values.yaml` assigns `runAsUser: 65532` and `runAsGroup: 65532` at pod level.

A synthetic different-EUID/same-GID test proved only that Linux group-write
permissions work. It did not prove that the product needs them. Because the
protocol has no independent authentication, owner-only `0o600` is the minimum
privilege justified by the shipped topology. A future group-shared mode needs an
explicit configuration surface, real chart wiring, and end-to-end coverage.

### Path ownership and stale recovery

Mode correction alone was insufficient. The old server unlinked any existing
path before `bind()` and unlinked whatever occupied the path at shutdown. That
allowed a second process to detach a live listener and allowed an exiting server
to remove another process's replacement.

The corrected lifecycle is:

1. Open an adjacent `.lock` file without following symlinks, validate that the
   opened and named objects are the same regular file, set it to `0o600`, and
   hold a non-blocking exclusive `flock` for the complete server lifetime.
2. Inspect `socket_path` using `lstat()`. Reject symlinks and every non-socket
   type without modifying them.
3. Refuse a connectable socket as active. Treat only `ECONNREFUSED` as a stale
   candidate, then `lstat()` again and unlink only if type, device, and inode
   are unchanged.
4. After `bind()`, record the published socket's device/inode identity, apply
   `0o600` without following symlinks, and verify identity again before listen.
5. On shutdown, unlink only when the current pathname is still a socket with
   the recorded identity.

The claim closes races among cooperating sidecar servers, including the narrow
bind-before-listen window in which `connect()` returns `ECONNREFUSED` for a live
owner. Unix has no portable atomic compare-and-unlink operation; an
uncooperative actor with parent-directory write permission can still race the
final identity check. Parent-directory permissions remain part of the security
boundary, and the implementation fails closed wherever the portable API allows.

### Adversarial coverage

The socket regression suite exercises:

- exact `0o600` mode and owner identity;
- a live second server and the bind-before-listen startup window;
- stale-socket recovery with a changed inode;
- symlink and ordinary-file refusal with contents preserved;
- a rebound live socket and a regular-file replacement surviving shutdown;
- readiness only after the real listener is published, with server-thread
  exceptions captured and asserted in the parent test;
- prompt shutdown of held connections and accept/registration races;
- a real different-EUID/same-GID peer receiving `EACCES` from the shipped
  owner-only endpoint.

## Verification summary

| Target / Check | Expected result |
|---|---|
| `compat/vmaf/tests/test_decorator_extended.py` | 26 passed, including spawn-process cases |
| `ai/sidecar/tests/test_socket_permissions.py` | 19 passed on POSIX; namespace-dependent cross-UID case may skip with an explicit reason |
| `ai/sidecar/tests/` | 93 passed on Python 3.14.7 / PyTorch 2.14.0 with warnings promoted to errors |
| Semgrep `p/python` on both source files | 0 text findings and 0 SARIF results |
| Nox | `compat_decorator` executes all 26 decorator tests |
| Hosted CI | Linux/macOS plus real Windows execution in `build.yml` |

Hosted alert closure still depends on the post-merge GitHub Code Scanning run;
the branch claims only local SARIF elimination, not remote closure before merge.

## References

- [ADR-1222](../adr/1222-code-scanning-alert-triage-and-scope.md)
- [ADR-1307](../adr/1307-sha256-memoization-cache-invalidation.md)
- [ADR-1309](../adr/1309-socket-path-ownership-and-owner-only-mode.md)
- Source: `req` — "well we have a lot of warnings lol as well..."
