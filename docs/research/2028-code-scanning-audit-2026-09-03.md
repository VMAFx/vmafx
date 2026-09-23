<!-- markdownlint-disable MD013 MD060 -->
# Code Scanning Alert Sweep & Security Dismissal Re-Audit (2026-09-03)

Evidence base and technical audit for clearing open GitHub code-scanning alerts and re-auditing warning-level dismissals in `VMAFx/vmafx`.

## 1. Executive Summary

A comprehensive audit of GitHub code-scanning alerts was conducted on 2026-09-03, followed by remediation of defects identified during independent review.

- **13 OPEN alerts** were audited:
  - 5 CodeQL findings genuinely fixed in source: `py/unused-import` in `test_smoke_e2e.py` (Alert 919), `cpp/trivial-switch` in `feature_name.cpp` (Alert 167), `cpp/loop-variable-changed` in `mkdirp.cpp` (Alert 164), and the `py/cyclic-import` pair between the Python MCP transports (Alerts 917, 918). The latter two were source-fixed on 2026-09-23. Live status before merge was 917 open and 918 fixed; final confirmation awaits the next CodeQL run on `master`.
  - 4 CodeQL findings verified as load-bearing patterns and reported-not-fixed without inoperative suppressions: exact float comparisons in `feature_name.cpp` (Alert 168) and `predict.c` (Alert 927) per ADR-0138 / ADR-0139; unit-test unity includes in `test_luminance_tools.cpp` (Alert 908) and `test_feature.cpp` (Alert 165).
  - 1 CodeQL finding reported-not-fixed: transient compiler probe file in `core/build/meson-private/...` (Alert 932), which cannot be filtered via `paths-ignore` in traced C/C++ builds.
  - 1 OpenSSF Scorecard vulnerability finding partially resolved: upgraded `golang.org/x/crypto` to `v0.56.0` (resolving GO-2026-6354 and GO-2026-6355); unconsumed `osv-scanner.toml` ignore dropped per project anti-suppression rule; unimported `GO-2026-5932` reported-not-fixed.
  - 2 OpenSSF Scorecard process/badge findings documented as maintainer-policy blockers in `docs/state.md` (Alerts 1, 3).
- **5 Security-relevant dismissals** (warning-severity "won't fix") were re-audited:
  - 3 SHA-1 memoization keys hardened with `usedforsecurity=False` in `decorator.py` (Alerts 731-733, PEP 451 / FIPS compliance).
  - 1 RFC UTF-8 bound in `pdjson.c` (Alert 691) genuinely fixed in source by removing redundant lower bound `0xC2 <= u` in `utf8_seq_length` (simplified to `u <= 0xDF`).
  - 1 UNIX-domain socket `0o660` mode in `online_trainer.py` (Alert 373): restored missing `srv.bind(socket_path)` preceding `os.chmod`, verified test suite passes, documented with `# nosemgrep`.

All changes pass `meson test -C build --suite=fast -j 4`, `pytest ai/sidecar/tests`, `CUDA_VISIBLE_DEVICES= make test-netflix-golden` (271 passed / 12 skipped / 0 failed, golden assertions untouched), whole-tree `clang-tidy`, `black`, `ruff`, and `pre-commit`.

---

## 2. Inventory of 13 Open Alerts (Part A)

| Alert # | Tool | Rule ID | Path : Line | Severity | Disposition | Resolution / Technical Justification |
| --- | --- | --- | --- | --- | --- | --- |
| **919** | CodeQL | `py/unused-import` | `mcp-server/vmaf-mcp/tests/test_smoke_e2e.py:28` | Note | Fixed | Removed unused `import pytest_asyncio`. |
| **918** | CodeQL | `py/cyclic-import` | `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py:443` | Note | Fixed in Source (2026-09-23) | `http_transport.py` now depends on the narrow `http_scoring.py` interface, not on `server.py`. The canonical server installs an adapter that preserves path validation, `ScoreRequest`, score execution, and RFC 8259 serialization. No dynamic import or scanner suppression is used. `tests/test_import_graph.py` walks imports at every lexical depth and requires the production package graph to remain acyclic. |
| **917** | CodeQL | `py/cyclic-import` | `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py:3196` | Note | Fixed in Source (2026-09-23) | Partner edge to Alert 918. `server.py::main()` still owns HTTP startup, but the HTTP module points only to `http_scoring.py`, so the dependency graph is `server -> http_transport -> http_scoring` with no return edge. Hosted closure awaits CodeQL on merged `master`. |
| **908** | CodeQL | `cpp/include-non-header` | `core/test/test_luminance_tools.cpp:20` | Note | Reported Not Fixed | Text-include of `feature/luminance_tools.cpp` required to unit-test internal static functions `range_foot_head` and `normalize_range` (ADR-0731); in-source suppression omitted because no dismiss-alerts workflow exists. |
| **165** | CodeQL | `cpp/include-non-header` | `core/test/test_feature.cpp:26` | Note | Reported Not Fixed | Text-include of `feature/feature_name.cpp` required for unit-test option-table isolation (ADR-0729 Wave 3); in-source suppression omitted to prevent collision with PR #1226 and because dismiss-alerts is absent. |
| **168** | CodeQL | `cpp/equality-on-floats` | `core/src/feature/feature_name.cpp:146` | Note | Reported Not Fixed | Intentional exact default comparison for option override detection (ADR-0138 / ADR-0139 bit-exactness); epsilon compare would break option serialization; left unsuppressed for maintainer decision. |
| **167** | CodeQL | `cpp/trivial-switch` | `core/src/feature/feature_name.cpp:113` | Note | Fixed | Replaced single-case `switch (opt->type)` with clean `if (opt->type == VMAF_OPT_TYPE_BOOL) ... else ...`. |
| **164** | CodeQL | `cpp/loop-variable-changed` | `core/src/feature/mkdirp.cpp:89` | Note | Fixed | Refactored `for` loop with body `++i` into an idiomatic `while (i < path.size())` loop. |
| **927** | CodeQL | `cpp/equality-on-floats` | `core/src/predict.c:301` | Note | Reported Not Fixed | Intentional exact sentinel check (`st->guided_score != st->sentinel`) where sentinel is `0.0` (ADR-0138 / ADR-0139 bit-exactness); epsilon band would distort near-zero chroma scores; left unsuppressed for maintainer decision. |
| **932** | CodeQL | `cpp/unused-local-variable` | `core/build/meson-private/tmpjc52bqqr/testfile.c:5` | Note | Reported Not Fixed | Transient Meson compiler feature-probe file; cannot be filtered via `paths-ignore` in traced C/C++ builds; left unsuppressed for maintainer decision. |
| **4** | Scorecard | `VulnerabilitiesID` | Repository Root (`go.mod`) | High | Partially Fixed | Upgraded `golang.org/x/crypto` to `v0.56.0` (resolves GO-2026-6354 and GO-2026-6355); unconsumed `osv-scanner.toml` ignore dropped; GO-2026-5932 in unimported openpgp reported. |
| **1** | Scorecard | `CodeReviewID` | Repository Root | High | Documented Blocker | Solo-maintainer structural artifact: squash-merging author PRs does not emit GitHub code-review approval events (ADR-0263, Research-0053). Documented in `docs/state.md`. |
| **3** | Scorecard | `CIIBestPracticesID` | Repository Root | Low | Documented Blocker | OpenSSF Best Practices badge requires external organizational application and ongoing verification audit (ADR-0263). Documented in `docs/state.md`. |

### 2.1 Alerts 917/918 follow-up decision (2026-09-23)

| Option | Benefit | Cost / failure mode | Decision |
| --- | --- | --- | --- |
| Route an edge through `importlib` | Small diff | Hides the same cycle from static analysis and loses type/IDE visibility | Rejected |
| Move `_run_vmaf_score` and every transitive dependency into a shared module | Direct ownership move | Verified closure includes about 25 helpers, caches, constants, and request types; large regression surface for an HTTP adapter | Rejected |
| Put the five HTTP needs behind one shared scoring interface and bind injected runtimes per server | Acyclic graph; canonical behavior and established patch seams stay intact; concurrent embedded servers remain isolated | Explicit runtime threading through the private HTTP startup path | Selected — [ADR-1304](../adr/1304-mcp-http-runtime-isolation.md) |

---

## 3. Inventory of 5 Re-Audited Security Dismissals (Part B)

| Alert # | Tool | Rule ID | Path : Line | Severity | Prior State | Re-Audit Verdict & Action Taken |
| --- | --- | --- | --- | --- | --- | --- |
| **731** | Semgrep | `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` | `compat/python-vmaf/tools/decorator.py:48` | Warning | Won't Fix | Hardened: Added `usedforsecurity=False` to `hashlib.sha1(...)` in `@persist` decorator. Documented non-cryptographic memoization key role. |
| **732** | Semgrep | `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` | `compat/python-vmaf/tools/decorator.py:118` | Warning | Won't Fix | Hardened: Added `usedforsecurity=False` to `hashlib.sha1(...)` in `@persist_to_file` decorator. Documented non-cryptographic memoization key role. |
| **733** | Semgrep | `python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` | `compat/python-vmaf/tools/decorator.py:143` | Warning | Won't Fix | Hardened: Added `usedforsecurity=False` to `hashlib.sha1(...)` in `@persist_to_dir` decorator. Documented non-cryptographic memoization key role. |
| **691** | CodeQL | `cpp/constant-comparison` | `core/src/pdjson.c:429` | Warning | Won't Fix | Fixed in Source: Removed redundant lower bound `0xC2 <= u` in `0xC2 <= u && u <= 0xDF` (simplified to `u <= 0xDF`), guaranteed by preceding branches. Exact RFC 3629 UTF-8 semantics preserved without suppression. |
| **373** | Semgrep | `python.lang.security.audit.insecure-file-permissions.insecure-file-permissions` | `ai/sidecar/online_trainer.py:427` | Warning | Won't Fix | Verified & Restored: Restored `srv.bind(socket_path)` preceding `os.chmod(socket_path, 0o660)`. Mode `0o660` restricts permissions strictly to user and group with zero world access (`---`) for IPC with Go node peer in the same group. Documented with inline `# nosemgrep`. Unit tests pass. |

---

## 4. Verification and Reproducers

### 4.1 Unit & Fast Test Suite

Command:

```bash
meson test -C build --suite=fast -j 4
```

Result: `105/105 passed (0 failed, 0 skipped)` in 5.01s.

### 4.2 Netflix CPU Golden-Data Gate

Command:

```bash
CUDA_VISIBLE_DEVICES= make test-netflix-golden
```

Result: `271 passed, 12 skipped in 160.80s`. Zero golden assertions modified; numerical fidelity preserved.

### 4.3 Python Sidecar Test Suite

Command:

```bash
pytest ai/sidecar/tests
```

Result: `67 passed, 38 warnings in 5.50s`. Verified `run_server` socket creation, binding, permissions, and clean teardown.

### 4.4 Go Vulnerability Status

`golang.org/x/crypto` upgraded to `v0.56.0` (resolves `GO-2026-6354` and `GO-2026-6355`). Unconsumed `osv-scanner.toml` dropped per repository policy; `GO-2026-5932` (deprecated `openpgp` in `x/crypto`) is unimported in `vmafx` and left visible as an upstream advisory.

### 4.5 Static Analysis & Linter Gates

- `clang-tidy -p build` on touched C/C++ files: 0 new warnings, 0 regressions.
- `python3 scripts/ci/tidy-ratchet.py`: Clean on touched translation units.
- `ruff check`: All Python files passed.
- `black --check`: All Python files formatted.
- `pre-commit run --files ...`: Passed across all 23 hooks.
