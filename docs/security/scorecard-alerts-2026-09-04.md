<!-- markdownlint-disable MD013 MD060 -->
# Security & OpenSSF Scorecard Alerts Audit (2026-09-04)

This document provides the security audit of OpenSSF Scorecard findings and
GitHub Code Scanning alerts evaluated during epic #1243 security cleanup for
the VMAFx repository.

## 1. OpenSSF Scorecard Alerts

The OpenSSF Scorecard analysis runs periodically and evaluates repository
supply-chain and maintenance posture. Three alerts were evaluated:

### Alert 1: `CodeReviewID` (Scorecard) — Maintainer-Only

- **Rule**: `CodeReviewID`
- **Severity**: High
- **Description**: Determines whether the project requires human code review
  before pull requests are merged.
- **Finding**: Score 0: 0/28 recent changesets approved by independent reviewers.
- **Status**: **Reported (Maintainer-Only Action)**
- **Audit Assessment**: VMAFx is developed primarily by the core maintainer with
  automated agent support. Enforcing multi-party mandatory code reviews requires
  organization-level branch protection rules (`require pull request reviews before
  merging` with approval from code owners/maintainers) on GitHub. This cannot be
  configured or enforced via in-repo files; it requires repository administrator
  action in GitHub repository settings once additional human maintainers are onboarded.

### Alert 3: `CIIBestPracticesID` (Scorecard) — Maintainer-Only

- **Rule**: `CIIBestPracticesID`
- **Severity**: Low
- **Description**: Determines whether the project has earned an OpenSSF Best
  Practices badge (passing, silver, or gold).
- **Finding**: Score 0: no effort to earn an OpenSSF best practices badge detected.
- **Status**: **Reported (Maintainer-Only Action)**
- **Audit Assessment**: Earning an OpenSSF Best Practices badge requires an
  external project registration at [bestpractices.dev](https://www.bestpractices.dev/)
  and manual verification of project policies and criteria. This is an administrative
  process managed by project leadership and cannot be automated or resolved by code changes.

### Alert 4: `VulnerabilitiesID` (Scorecard) — Remediated via `osv-scanner.toml`

- **Rule**: `VulnerabilitiesID`
- **Severity**: High
- **Description**: Determines if the project has open, unfixed vulnerabilities in its
  codebase or dependencies using OSV.
- **Finding**: OSV advisory [GO-2026-5932](https://osv.dev/GO-2026-5932) reported
  against the unmaintained `golang.org/x/crypto/openpgp` subpackage.
- **Status**: **Ignore rule proposed (`osv-scanner.toml`) — maintainer decides**
- **Audit Assessment**: `golang.org/x/crypto` was upgraded to `v0.56.0` on master,
  which resolved vulnerabilities `GO-2026-6354` and `GO-2026-6355`. However,
  `GO-2026-5932` is an informational advisory noting that the `openpgp` subpackage
  within `x/crypto` is deprecated and unmaintained. VMAFx does not import or use
  `openpgp` anywhere in its Go codebase. Per OSV-Scanner standard remediation,
  an `osv-scanner.toml` ignore rule is placed next to `go.mod` documenting that
  the deprecated subpackage is unimported and unreachable in VMAFx
  (`go mod why golang.org/x/crypto/openpgp` → `main module does not need package`;
  `golang.org/x/crypto` is pulled in transitively via `golusoris/crypto` → `argon2id`
  → `x/crypto/argon2`). The advisory has no fixed version (`introduced: 0`,
  the subpackage is unmaintained by design), so a dependency bump cannot clear it.
  Scorecard's Vulnerabilities check calls `osvscanner.DoScan` on the checkout
  (`clients/osv.go`), and osv-scanner loads an `osv-scanner.toml` found next to
  the manifest it scans, so the ignore is expected to be honoured on the next
  weekly Scorecard run; that is not yet confirmed. Whether to keep this ignore
  is the maintainer's call — nothing was dismissed in the Security tab.

```toml
# osv-scanner configuration
# Ignored vulnerabilities

[[IgnoredVulns]]
id = "GO-2026-5932"
reason = "Unmaintained/deprecated subpackage golang.org/x/crypto/openpgp is an indirect dependency not imported or reachable anywhere in the vmafx codebase."
```

---

## 2. Code-Scanning Alerts Sweep

A comprehensive sweep of all open GitHub Code Scanning alerts was conducted.
Alerts were resolved either through code fixes or classified as `reported-not-fixed`
with justifications.

### 2.1 Resolved Code Defects

| Alert(s) | Tool | Rule | File(s) | Resolution |
| --- | --- | --- | --- | --- |
| none (proactive hardening) | CodeQL | `cpp/integer-multiplication-cast-to-long` (history) | `core/src/feature/iqa/convolve.c`, `core/src/feature/moment.c`, `core/src/feature/psnr.c` | Widened the integer *index* operands to `(ptrdiff_t)` before multiplication (`pic[(ptrdiff_t)i * stride_ + j]`); numerically inert, same element addressed. No open alert of this rule exists. The six alerts the rule ever raised on these files (30, 31, 33, 706, 707, 708) sit on `float * float` products accumulated into `double`; they were dismissed as false positives in June 2026 and stay dismissed, because widening those operands changes the single-rounded product and breaks the SIMD bit-exactness contract (ADR-0138, ADR-0179). An earlier draft of this branch applied that `(double)` cast and was reverted. |
| 372 | Semgrep OSS | `python.lang.security.audit.dangerous-subprocess-use-tainted-env-args` | `ai/scripts/extract_ugc_features.py` | Replaced false-positive dismissal with strict input validation on dimensions (`w > 0`, `h > 0`), threads (`n_threads > 0`), paths (null bytes / empty checks), and `VMAF_TINY_AI_SCRATCH` directory confinement. Added unit tests in `ai/tests/test_extract_ugc_features.py`. |
| 965 | CodeQL | `py/unused-import` | `mcp-server/vmaf-mcp/tests/test_parity_argv.py` | Removed unused `import pytest`. |
| 960 | CodeQL | `cpp/missing-header-guard` | `core/tools/spinner.h` | Added `#ifndef VMAF_SPINNER_H` / `#define VMAF_SPINNER_H` / `#endif // VMAF_SPINNER_H` guards. |
| 938, 939 | CodeQL | `cpp/unused-static-variable`, `cpp/unused-local-variable` | `core/tools/cli_parse.cpp` | Split `usage()` into non-variadic and variadic overloads so parameter packs are never unused in zero-argument calls. |
| 951 | CodeQL | `cpp/commented-out-code` | `core/test/test_model_feature_overload_ownership.c` | Rephrased comment containing `` `return`ed `` that triggered CodeQL's commented-out code heuristic. |
| 954 | CodeQL | `cpp/constant-comparison` | `core/src/pdjson.c` | Removed redundant lower-bound comparisons (`u <= 0xEF`, `u <= 0xF4`) in UTF-8 sequence length validation. |

### 2.2 Documented: Reported, Not Fixed

The following findings represent deliberate architectural decisions, test harnesses,
or upstream-mirror behaviors:

1. **Alert 969 (`cpp/unused-local-variable` in `core/build/meson-private/.../testfile.c`)**:
   - Ephemeral compiler probe file generated automatically by Meson during feature detection (`meson setup`). Not repository source code.
     The same pattern produced alerts 920–926, 928 and 929 between 2026-08-31 and 2026-09-02, each auto-closed by the next
     scan once its random temp directory disappeared; 969 will close the same way. A durable fix would keep the CodeQL
     job's `meson setup` scratch out of the traced build (it is a CI-workflow change, not a code defect).
2. **Alerts 955, 943, 908 (`cpp/include-non-header` in test translation units)**:
   - `core/test/test_model.c` includes `model.c` to test static model definitions directly (ADR-0278 white-box pattern).
   - `core/test/test_feature.cpp` includes `feature/feature_name.cpp` per ADR-0729 unity-include pattern.
   - `core/test/test_luminance_tools.cpp` includes `feature/luminance_tools.cpp` for internal test access.
3. **Alerts 927 & 168 (`cpp/equality-on-floats` in `core/src/predict.c` and `core/src/feature/feature_name.cpp`)**:
   - Exact float comparisons (`==`) required for numerical parity and bit-exact contract under ADR-0138 and ADR-0139.
4. **Alerts 917 & 918 (`py/cyclic-import` in `mcp-server/vmaf-mcp`)**:
   - `http_transport.py` and `server.py` use function-local imports that do not execute at module initialization time. Documented in Research-2028; refactoring requires extracting shared symbols to a separate module.
5. **Alerts 947, 948, 949 (`python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1` in `compat/python-vmaf/tools/decorator.py`)**:
   - Hardened with `usedforsecurity=False` in commit `93ffd7333`. Used solely for non-cryptographic memoization cache keys in upstream-compatible Python harness.
6. **Alert 946 (`python.lang.security.audit.insecure-file-permissions` in `ai/sidecar/online_trainer.py`)**:
   - UNIX domain socket permissions set to `0o660` to permit authorized local IPC group access. Audited and verified in Research-2028.

---

## 3. CodeQL re-run on `master`

`Security Scans — Semgrep / CodeQL / Gitleaks / Dependency Review`
(`.github/workflows/security-scans.yml`) already carries `workflow_dispatch:`.
A manual run on `master` (run id 33920556824, 2026-09-04 21:19 UTC, success)
refreshed the alert set before this audit; the open list above is the
post-run state. The fixes in this branch close alerts 938, 939, 951, 954, 960
and 965 once the branch's own scan runs; the `cpp/integer-multiplication-cast-to-long`
history (alerts 30, 31, 33, 706, 707, 708) is unchanged — those remain
maintainer-dismissed false positives, see §2.1.
