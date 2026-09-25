<!-- markdownlint-disable MD013 MD060 -->

# ADR-1307: Pure SHA-256 memoization keys with cold cache invalidation

- **Status**: Accepted
- **Supersedes**: Partially supersedes [ADR-1222](1222-code-scanning-alert-triage-and-scope.md) (only for Alerts 947–949 SHA-1 keep-open disposition)
- **Date**: 2026-09-24
- **Deciders**: Lusoris
- **Tags**: `python`, `security`, `concurrency`, `compatibility`

## Context

GitHub Code Scanning reported three warning-level alerts from Semgrep rule
`python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1`:

- **Alert 947**: `compat/python-vmaf/tools/decorator.py` in `@persist`
- **Alert 948**: `compat/python-vmaf/tools/decorator.py` in `@persist_to_file`
- **Alert 949**: `compat/python-vmaf/tools/decorator.py` in `@persist_to_dir`

Upstream Netflix VMAF used `hashlib.sha1(..., usedforsecurity=False)` to generate
cache key digests from serialized function names and arguments.

[ADR-1222](1222-code-scanning-alert-triage-and-scope.md) previously documented these
alerts under its disposition table as "Correct as written", intending to keep them
open with in-code `# nosemgrep` comments. However, Semgrep CLI records in-code
suppressions in SARIF output under `"suppressions": [{"kind": "inSource"}]`.
Because `security-scans.yml` uploads raw SARIF to GitHub Code Scanning, GitHub does
not close alerts that appear in SARIF regardless of `inSource` annotations.
Consequently, in-code suppressions cannot close alerts 947–949.

An architectural audit of `compat/python-vmaf/tools/decorator.py` established:

1. `decorator.py` provides runtime memoization for python-side execution only.
2. In-memory `@persist` caches exist only for process lifetime and cannot survive restarts.
3. For `@persist_to_file` and `@persist_to_dir`, cached entries represent ephemeral,
   reconstructible intermediate function evaluations.
4. Durable pipeline artifacts in VMAFx (such as `.vmaf` output stores, `Result`
   serialization in `compat/python-vmaf/core/result.py`, and `Asset` hashing in
   `compat/python-vmaf/core/asset.py` / `executor.py`) do not use `decorator.py`.
5. Core scoring routines, C library computations, and Netflix golden assertions
   in `python/test/` are completely independent of `decorator.py`.
6. Concurrency bugs existed in `decorator.py`: `persist_to_file` wrote JSON
   directly to the destination with `open(file_name, "wt")`, so interruption
   could expose a partial file and uncoordinated processes could clobber entries.

## Decision

1. **Pure SHA-256 Memoization**: Replace `hashlib.sha1` with
   `hashlib.sha256(..., usedforsecurity=False)` across `@persist`,
   `@persist_to_file`, and `@persist_to_dir`. Cache digests are now 64-character
   hex strings.
2. **Clean Cold Invalidation**: Accept cold invalidation of legacy memoization
   caches on upgrade. Pre-existing SHA-1 cache entries on disk miss cleanly,
   prompting recomputation and persistence under SHA-256 keys. No legacy SHA-1
   fallback, dual-hash read-through, or `# nosemgrep` suppression is retained.
   This eliminates alerts 947–949 from SARIF (0 findings).
3. **Explicit Partial Supersession of ADR-1222**: ADR-1222's disposition classifying
   alerts 947–949 as keep-open / "Correct as written" is superseded. Alert 946
   is governed separately by [ADR-1309](1309-socket-path-ownership-and-owner-only-mode.md).
4. **Concurrency and Cross-Process Safety**:

   - Atomic replacement: `_write_json_cache_atomic` creates unique temporary files
     using `tempfile.mkstemp(dir=file_dir, prefix=f".{base_name}.", suffix=".tmp")`
     with error cleanup (`os.close` and `os.unlink`), followed by atomic `os.replace`.
   - In-process recursion safety: `threading.RLock()` serializes threads while
     allowing re-entrant acquisition for recursive dynamic programming algorithms.
   - Cross-process synchronization: `_file_lock` implements a re-entrant cross-process
     file lock over `f"{file_name}.lock"` using `fcntl.flock` on POSIX and
     `msvcrt.locking` on Windows.
   - Cache merging: `persist_to_file` reloads and merges disk state under `_file_lock`
     on cache misses prior to atomic write, preventing concurrent processes from
     clobbering each other's keys.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Clean SHA-256 cold invalidation (chosen)** | 0 Semgrep findings in SARIF; genuinely closes alerts 947–949 on GitHub; eliminates dead hashing paths and legacy baggage | One-time cache miss on legacy disk caches | Chosen: memoization is ephemeral and reconstructible; Netflix goldens are unaffected. |
| **Read-through fallback with SHA-1 keep-open** | Preserves pre-existing legacy on-disk cache hits | Retains SHA-1 code and `# nosemgrep` comments; leaves findings in SARIF as `inSource` suppressions, keeping GitHub alerts open | Defeats the primary goal of resolving code-scanning alerts at source. |
| **Dual hashing (check SHA-256, fall back to SHA-1)** | Seamless upgrade transition | Requires ongoing execution of SHA-1, triggering Semgrep alerts on fresh code | Unacceptable under whole-codebase security standards. |
| **Third-party locking libraries (`filelock`, `portalocker`)** | Pre-packaged cross-platform file locking | Introduces new external dependencies into upstream-compat tooling; fails offline/distroless builds | Rejected in favor of standard-library `fcntl` and `msvcrt` implementations. |
| **Standard non-reentrant `threading.Lock`** | Simpler lock structure | Deadlocks immediately when memoized functions recurse (e.g. Fibonacci dynamic programming) | Incompatible with common memoization recursion patterns. |
| **PID-based temporary files (`target.tmp.<pid>`)** | Would avoid partial destination writes without a new dependency | Multiple threads within one process share a PID, so this hypothetical design would collide | Rejected during design; the base implementation wrote the destination directly and never used PID temp files. |

## Consequences

- **Positive**:
  - Semgrep alerts 947, 948, and 949 are eliminated at source with 0 findings in SARIF.
  - Multi-threaded and multi-process cache persistence is verified race-free and clobber-free.
  - Recursive memoization functions operate without deadlocks.
- **Negative**:
  - Pre-existing on-disk cache files with SHA-1 keys will be orphaned and ignored.
- **Neutral / follow-ups**:
  - ADR-1222 disposition table updated to record supersession of alerts 947–949.

## References

- [ADR-1222: In-code suppressions do not close code-scanning alerts; scope the scan instead](1222-code-scanning-alert-triage-and-scope.md)
- [Research-2095: Semgrep Warning Alerts 946–949 Audit and Resolution](../research/2095-semgrep-warning-alerts-946-949-audit.md)
- [ADR-1309: Owner-only sidecar socket with identity-checked lifecycle](1309-socket-path-ownership-and-owner-only-mode.md)
- [ADR-1278: Bounded process execution and safe parallelism](1278-python-safe-parallel-execution.md)
- Source: `req` — "well we have a lot of warnings lol as well..."
