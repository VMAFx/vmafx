<!-- markdownlint-disable MD013 MD060 -->
# VMAFx — Engineering Principles

This page defines the non-negotiable standards for code merged to `master`.
Read it before changing code, scripts, schemas, configuration or build definitions.
VMAFx extends Netflix's Video Multi-Method Assessment Fusion (VMAF) engine;
these standards cover every implementation language in the fork.

The C-oriented requirements in sections 1–7 use `.clang-tidy`, `.cppcheck-suppressions.txt`,
`.semgrep.yml`, `.pre-commit-config.yaml` or
`.github/workflows/{lint-and-format,security-scans,supply-chain}.yml`.
Sections [8](#8-languages) and [9](#9-applicability-matrix) distinguish
existing language gates from missing or partial enforcement. Missing gates
are follow-up work in [#2438](https://github.com/VMAFx/vmafx/issues/2438);
existing diagnostic debt remains in [`docs/state.md`](state.md).

**Scope (ADR-1142).** Every principle and every gate below applies to the
*whole* tree — upstream-mirror Netflix code, vendored libraries, fork-added
code, CUDA / HIP / SYCL / Metal kernels, SIMD paths, tests, tools and
scripts alike. Language-specific adaptations are defined in section 9;
origin does not exempt code from the applicable rules. The
one invariant that outranks the standards is numerical correctness as pinned
by the Netflix golden assertions (the golden-data section of
[the agent reference](development/agent-reference.md#netflix-golden-data-gate), and
[section 3.1](#31-netflix-golden-data-gate) below). Whole-tree lint debt is
measured and may only decrease (`make tidy-ratchet`, see
[docs/development/ci.md](development/ci.md#whole-tree-lint-ratchet-adr-1142)).

## 1. Coding

### 1.1 NASA/JPL "Power of 10" rules (adapted for C)

1. **Simple control flow.** No `goto`, no `setjmp`/`longjmp`, no recursion
   (static or dynamic).
2. **Bounded loops.** Every loop has a statically-verifiable upper bound on
   iterations.
3. **No dynamic allocation in hot paths.** `malloc`/`calloc`/`realloc` are
   permitted at
   initialization only; frame-loop code paths are allocation-free.
4. **Short functions.** Function bodies ≤ 60 lines (a single printed page).
   Enforced by
   `readability-function-size` in clang-tidy with `LineThreshold: 60`.
5. **Assertion density.** ≥ 2 runtime assertions per function on average. Use
   `assert()`
   for invariants; in hot loops where `assert()` cost matters, use
   `VMAF_ASSERT_DEBUG()`
   which compiles away in release.
6. **Minimal scope.** Declare variables at the smallest possible scope.
7. **Check return values.** Every non-void function return must be checked and
   handled. Casting a result to `void` does not handle an error. Enforced by
   `cert-err33-c`.
8. **Preprocessor restraint.** Only header inclusion (`#include`) and simple
   macros. No
   token pasting, no recursive macros, no conditional compilation beyond
   platform/feature
   toggles established at build-system level.
9. **Pointer restraint.** Single level of dereferencing in most cases. No
   function
   pointers except where essential for dispatch (feature extractor registry,
   SIMD
   runtime selection).
10. **Max strictness.** Zero compiler warnings (HISS-10). `core/meson.build`
    builds at
    `warning_level=2` (`-Wall -Wextra`); it does not set `-Wpedantic` or
    `-Werror`. The
    checks listed under `WarningsAsErrors` in `.clang-tidy` fail the build.
    Static
    analyzers (clang-tidy, cppcheck, scan-build) must finish with zero findings
    at
    enabled levels.

### 1.2 JPL Institutional Coding Standard for C — applicable subset

The JPL-C-STD is the 31-rule superset of Power of 10. The following rules extend
1.1 and
are additionally enforced. The rule numbers are the JPL numbers.

#### Preprocessor and headers

- **Rule 11.** Place `#include` directives only at the top of a file.
- **Rule 12.** Do not place executable code inside a `#include`d file.
- **Rule 13.** Do not use reserved names (names beginning with `_` followed by
  uppercase
  letter, or `__`).
- **Rule 19.** All non-trivial types used in a function's public interface must
  be
  defined in a header.
- **Rule 31.** All headers must be self-contained (include what they need) and
  include-guard protected.

#### Casts, expressions and initialisation

- **Rule 14.** All type casts must be explicit.
- **Rule 15.** The evaluation order of operands must not matter. No `i++` or
  `++i` used
  within a larger expression; split into separate statements.
- **Rule 16.** No assignments inside expressions.
- **Rule 17.** All variables must be initialized before use.
- **Rule 22.** No side effects in conditional expressions.
- **Rule 20.** Use `const` aggressively. Anything not modified is `const`.

#### Globals and linkage

- **Rule 18.** All global variables must have a single declaration, and it must
  be in the
  file that owns them.
- **Rule 21.** Use `static` on every symbol not referenced outside its
  translation unit.

#### Switch and control flow

- **Rule 23.** All switch statements must have a default case.
- **Rule 24.** No fall-through in switch statements except where explicitly
  marked with
  `/* FALLTHROUGH */` or `__attribute__((fallthrough))`.
- **Rule 25.** Functions must have a single exit point where practical. Early
  returns are
  permitted for validation at function entry.
- **Rule 26.** Use of pointer arithmetic is restricted to array traversal within
  bounded
  loops.

#### Signals, variadics and library functions

- **Rule 27.** No use of `<setjmp.h>`, `<signal.h>` handlers that do real work,
  or any
  non-async-signal-safe function inside a signal handler.
- **Rule 28.** No variadic functions except standard `printf`/`scanf` family.
- **Rule 29.** No use of `<stdarg.h>` macros outside of printf/scanf wrappers.
- **Rule 30.** Banned functions: `gets`, `strcpy`, `strcat`, `sprintf`, `strtok`
  (non-reentrant), `atoi`, `atol`, `atof` (no error reporting), `rand`
  (non-cryptographic
  and non-reproducible), `system` (outside build scripts). Use: `fgets`,
  `strncpy` (with
  explicit null-termination) / `snprintf` / `strlcpy`, `snprintf`, `strtok_r`,
  `strtol`
  errno checking, `arc4random` or a seeded `xorshift`, direct process calls with
  arg arrays.

### 1.3 SEI CERT C & CERT C++

Full compliance required for:

- **INT (Integers)** — no signed overflow, always check `size_t` arithmetic,
  narrow casts
- **STR (Strings)** — no off-by-one, always check buffer bounds
- **MEM (Memory management)** — every `malloc` has a matching `free`; no
  double-free; no
  use-after-free (ASan enforced)
- **FIO (I/O)** — check every `fopen`, `fread`, `fwrite`, `fclose` return
- **EXP (Expressions)** — no undefined behavior; no sequence-point violations
- **CON (Concurrency)** — use atomics correctly (`_Atomic`); no data races (TSan
  nightly)
- **ENV (Environment)** — never trust `getenv` input without validation

Enforcement: `cert-*` checks in `.clang-tidy` all enabled. The noisy
`clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling` subset
(Microsoft C11 `_s` functions) is explicitly disabled — it does not map to
any portable POSIX API.

### 1.4 MISRA C:2012 (informative subset)

Applied where it does not conflict with existing libvmaf conventions:

- **Rule 8.5** (one external declaration per identifier in a single file)
- **Rule 10.1–10.8** (essential type rules — stricter than standard C integer
  promotions)
- **Rule 17.7** (function return values must be used) — matches Power of 10 #7.
  MISRA
  allows an explicit `void` discard; the project rule does not (section 1.1,
  rule 7).
- **Rule 21.x** (banned functions) — matches JPL rule 30

Not enforced as PR-blocking; informational in review.

### 1.5 Style

- **C:** Existing libvmaf conventions preserved (K&R braces, 4-space indent,
  100-char
  columns). Codified in [.clang-format](../.clang-format).
- **C++ (SYCL code):** same as C where applicable; RAII encouraged for
  queue/context
  wrappers; no exceptions in hot paths.
- **Python:** PEP 8 + black (line-length 100) + ruff (import sorting via ruff's
  `I` ruleset — ADR-1126). Codified in
  [pyproject.toml](../pyproject.toml).
- **CUDA (`.cu`):** follows C style; kernel names `kernel_*`; device helpers
  `device_*`.
- **Shell:** shfmt + shellcheck; `#!/usr/bin/env bash`; `set -euo pipefail`.

### 1.6 Headers shared by C and C++

**Rule.** A type that both a C and a C++ translation unit read has one
definition, in
the C form. An enum in such a header does not get a fixed underlying type for
C++ only
(`#ifdef __cplusplus` / `enum E : uint8_t`).

**Why.** C cannot spell the fixed type on the required MSVC lane, so the enum
would be
`int`-sized in C and narrower in C++. A value passed by value, stored in a
shared struct
or read through a pointer would then be read with the wrong size
([ADR-1470](adr/1470-c-cxx-shared-enum-one-definition.md)).

**Instead.** Suppress `performance-enum-size` and `modernize-use-using` on the
definition
with a citation.

**Exception.** An enum whose size is part of an ABI may state `: unsigned int`
for C++
when it carries a `UINT_MAX` enumerator that holds the C enum to the same four
bytes
(`core/src/model.h`).

**Guard.** `core/test/test_c_cxx_enum_definition_contract.py` checks every
header a `.c`
file under `core/` includes.

## 2. Security

See [SECURITY.md](../SECURITY.md) for reporting policy.

### 2.1 Input validation at boundaries

- All CLI inputs validated at parse time (see `core/tools/cli_parse.cpp`).
- All public libvmaf API entry points validate pointer non-null + struct
  version.
- Video frame dimensions bounded; no negative or zero-size frames accepted.
- File paths validated for NULL and checked with `access()` before open.

### 2.2 Memory safety

Enforced today (`.github/workflows/sanitizers.yml`):

- **ASan + UBSan** on every PR (required contexts `Sanitizers (address)`,
  `Sanitizers (undefined)` and `Sanitizers ASan+UBSan`).
- **TSan** (`Sanitizers (thread)`) on pushes to `master`.
- **libFuzzer + ASan** nightly at 04:30 UTC.

Target hardening flags. These are the intended release-build settings; none of
them is
defined in `core/meson.build`, `core/src/meson.build` or the container build
files yet:

- `_FORTIFY_SOURCE=3` in release builds.
- Stack protector `-fstack-protector-strong`.
- PIE (`-fPIE`, `-pie`) for all binaries.
- RELRO + BIND_NOW: `-Wl,-z,relro,-z,now`.
- valgrind memcheck on release-candidate audit.

### 2.3 Banned functions

See JPL rule 30 above — enforced by `.semgrep.yml` custom rules +
`bugprone-unsafe-functions` in clang-tidy.

### 2.4 Dependency auditing

Enforced today:

- **Dependency Review**, **Gitleaks**, **Semgrep** and **CodeQL** run on every
  PR
  (`.github/workflows/security-scans.yml`; CodeQL uses the `security-extended`
  and
  `security-and-quality` suites from `.github/codeql-config.yml`).
- **Renovate** (`renovate.json`) opens dependency PRs on weekdays before 06:00
  Europe/Vienna and pins GitHub Actions to digests.
- `osv-scanner.toml` configures the OSV scanner.

Target scanners. These are named as goals; no workflow in the tree runs them as
a PR
gate yet: **trivy** (Dockerfile and filesystem CVEs) and **pip-audit** (Python
dependencies).

## 3. Quality gates (required for PR merge to master)

- ✅ `make lint` — zero clang-tidy / cppcheck / ruff / black findings
- ✅ `make test` — all unit + integration tests pass with ASan + UBSan
- ✅ `make sec` — zero semgrep findings (it runs `semgrep` with `.semgrep.yml`,
  the
  `p/cwe-top-25` and `p/cert-c-strict` rule sets). Gitleaks runs as the
  `Gitleaks` CI
  gate and as a pre-commit hook. `flawfinder-new` checks changed C/C++ files;
  `bandit-new` compares Python files with their merge-base copies. Pre-commit
  scans the working tree; pre-push scans the outgoing commit, including its
  configuration. The default comparison branch follows `origin/HEAD`, with
  `origin/master` as fallback; `--base-branch` overrides it. Both reject only
  new findings. The Python check uses each revision's [Bandit patterns](../bandit.yml);
  a missing base configuration uses the current configuration. A configuration
  change scans all tracked Python files, including unchanged sources. Codacy
  reads that list on each analysed branch after its configuration-file option
  is enabled. Editing that file changes Codacy's patterns;
  without it, Codacy uses its UI patterns. Refresh the list from Codacy's
  enabled Bandit IDs, then run the planted recall test in
  `scripts/ci/tests/test_check_bandit_new.py`.
- ✅ CodeQL — zero security-and-quality issues at `security-extended` suite
- ✅ Conventional commit messages (enforced by commit-msg hook)
- ✅ CI matrix green on Linux/macOS/Windows
- ✅ Netflix source-of-truth golden tests pass (CPU, 3 pairs: 1 normal + 2
  checkerboard)
- ✅ SYCL parity (ADR-1177) — when `SYCL_ARC_RUNNER_ENABLED=true`, the required
  Arc A380 lane runs the SYCL unit suite and the CPU↔SYCL `float_ssim` matrix
  cell; a missing or offline enabled runner fails loudly. Broader CUDA/SYCL
  sweeps remain explicit local or backend-specific test runs. See
  [development/cross-backend-gate.md](development/cross-backend-gate.md).
- ✅ Coverage ≥ 70% overall, ≥ 90% for critical code (validation, parsing,
  crypto-adjacent); floors live in `scripts/ci/coverage-check.sh`
  ([ADR-0922](adr/0922-coverage-ratchet-aggressive.md))
- ✅ Touched-file lint-clean rule (ADR-0141) — every hunk in the PR's diff
  against its merge base must be lint-clean to the fork's strictest profile;
  `// NOLINT` is reserved for load-bearing invariants and must cite the ADR or
  digest that forces it.
- ✅ State-tracking rule (ADR-0165) — every PR that opens, closes, or rules
  out a Netflix upstream report against the fork updates
  [`state.md`](state.md) in the same PR.
- ✅ FFmpeg-patches sync rule
  ([ADR-1240](adr/1240-ffmpeg-release-patch-lifecycle.md), rule 11 of the
  [agent hard rules](development/agent-hard-rules.md)) — every PR touching a
  libvmaf C-API surface, CLI flag, `meson_options.txt` entry, or any other
  interface that the in-tree `ffmpeg-patches/` patches consume updates the
  relevant patch file in the same PR.

### 3.1 Netflix golden-data gate

The fork preserves the three Netflix-authored reference test pairs as the
canonical
ground-truth gate for VMAF numerical correctness (CPU only):

| # | Type          | Reference                                      | Distorted                                      |
|---|---------------|------------------------------------------------|------------------------------------------------|
| 1 | normal        | `src01_hrc00_576x324.yuv`                      | `src01_hrc01_576x324.yuv`                      |
| 2 | checkerboard  | `checkerboard_1920_1080_10_3_0_0.yuv`          | `checkerboard_1920_1080_10_3_1_0.yuv` (1-px)   |
| 3 | checkerboard  | `checkerboard_1920_1080_10_3_0_0.yuv`          | `checkerboard_1920_1080_10_3_10_0.yuv` (10-px) |

YUVs live in `python/test/resource/yuv/` (gitignored; provision them with
[`scripts/test/fetch-test-yuvs.sh`](development/test-fixtures.md)). Golden expected
scores are hardcoded `assertAlmostEqual` assertions in `python/test/` (primarily
`quality_runner_test.py`, `vmafexec_test.py`,
`vmafexec_feature_extractor_test.py`,
`feature_extractor_test.py`, `result_test.py`). **These assertions are never
modified.**
They run in CI as a required status check on every PR. They are not run as a
pre-commit
hook because their runtime is longer than the acceptable pre-commit latency
budget.

`make test-netflix-golden` runs them against an isolated native build
([ADR-1317](adr/1317-golden-gate-build-isolation.md)).
`make test-netflix-golden-arm64` runs the same assertions against an aarch64
cross build under qemu-user, with GCC or clang
([ADR-1461](adr/1461-strict-fp-every-translation-unit.md),
[ARM backend guide](backends/arm/overview.md#running-the-golden-gate-for-aarch64-on-an-x86-host)).

Fork-added tests (SYCL, CUDA, SIMD snapshots, performance benchmarks) live in
separate
files and directories, and must not modify or override Netflix golden behavior.

## 4. Deterministic builds

- `SOURCE_DATE_EPOCH` respected throughout the build
- All build-time random data (tempdir names, etc.) is seeded from
  `SOURCE_DATE_EPOCH`
- Meson pinned to a specific minor version via `dev-setup` scripts
- Compiler/linker flags logged and archived per release

## 5. Supply chain

See the tracked [release](development/release.md) and
[repository-security](development/repository-security.md) runbooks:

- **SLSA v1 build provenance** on every release: a GitHub artifact attestation
  signed through Sigstore ([ADR-1356](adr/1356-release-provenance-attest.md))
- **CycloneDX + SPDX SBOMs** on every release
- **Keyless cosign** signatures via Sigstore / GitHub OIDC
- **Transparency log** (Rekor) makes signatures publicly verifiable

## 6. Compliance targets

- **OpenSSF Best Practices** — truthful Passing assessment first; Silver/Gold
  are
  later goals, not earned status. Registration and evidence are active work.
- **OpenSSF Scorecard** — unrounded ≥ 8.5 required on master, with exact-head
  full-report verification and explicitly separate PR file checks per
  [ADR-1247](adr/1247-scorecard-exact-head-gates.md) and the
  [operator guide](development/ossf-scorecard.md). Scanner errors fail; the
  precise no-release state remains visibly unassessed, never signed.
- **OWASP ASVS** — Level 2 applicable controls (auth/authz not applicable —
  library)
- **NIST SSDF** — PW.4 (design), PW.5 (implement), PW.7 (review), PW.9 (archive)
  aligned

## 7. Non-goals

We explicitly do **not** pursue:

- Full MISRA C compliance (too restrictive for numerical code — informative
  subset only)
- Claiming a Best Practices badge before its criteria and external status are
  verified; no calendar deadline substitutes for that evidence
- FIPS 140 (no cryptography in this library)
- DO-178C (aerospace software — out of scope)

## 8. Languages

This section owns language roles, standards and gate coverage; the
[language setup guide](development/languages.md) owns installation and
workspace recipes. The inventory below follows tracked source at
`1a0f497e2925cca8a3566ba52905707cf7e995b0`, read on 2026-11-07 with
`git ls-files`. It includes executable languages, build languages, schemas
and documentation markup. File contents and their consumer determine the
language: `scripts/ci/exact_twins.d/*.hip` are data, not HIP kernels.
Future languages need an explicit policy and coverage record when introduced.
[ADR-3385](adr/3385-principles-per-language-matrix.md) records this docs-only
slice of [#2438](https://github.com/VMAFx/vmafx/issues/2438).

**How to use this section.** Start with the language below, then read its
C-core mapping in [section 9](#9-applicability-matrix). Rule numbers refer to
section 1.1. C syntax restrictions apply only where that construct exists;
the safety obligations survive in the language's error, ownership and
concurrency model. JPL/CERT rules in sections 1.2–1.4 remain binding at
native boundaries; they are not a claim of CERT certification for other
languages. Installation belongs in the language setup guide.

**Gate commands are a reference, not an execution receipt.** Run from the
repository root unless stated otherwise, with the declared toolchain and
build prerequisites. A configured backend build must include the changed
source. Record tool/version, command, exit code and skipped coverage;
compiling a kernel does not prove it ran on hardware. Where no command
exists, the section says **no gate yet** rather than inventing one.

**Common contract.** HISS-10 requires compiler, linter and formatter
findings to fail in every language; a ratchet, advisory check or parser is
not proof of that complete policy. HISS-15 requires positive, negative and
boundary cases for public interfaces. Section 9.1 lists missing enforcement,
including rules with only partial checks. The
[HISS evidence catalog](../.config/hiss/coverage.yaml) limits what
`praetorctl hiss coverage --verify` proves; replaying that catalog does not
prove every rule in every language. Existing exceptions remain explicit in
[the standards manifest](../.standards.yaml) and
[declared lint exceptions](../.config/lint-exceptions.d/ruff.toml).

### 8.1 C

- **C core:** All ten C-core rules apply directly, together with the JPL, CERT and informative MISRA subsets. Shared C/C++ headers retain section 1.6.
- **Role and standard:** engine, extractors, public headers and tests in
  `core/`, plus inherited MEX C in `compat/python-vmaf/matlab/`. Sections
  1–7 retain the C rules. New fork C targets C23; the actual fallback list
  (`c23,c2x,c17,none`) and MSVC handling live in
  [core/meson.build](../core/meson.build).
- **Gates:** [the hooks](../.pre-commit-config.yaml) run clang-format with
  [.clang-format](../.clang-format). `make lint-c` uses
  [lint-configured.py](../scripts/ci/lint-configured.py), [.clang-tidy](../.clang-tidy)
  and [.cppcheck-suppressions.txt](../.cppcheck-suppressions.txt).
  `python3 scripts/ci/run_meson_test.py -- -C build` runs
  [registered tests](../core/test/meson.build); `make test-netflix-golden`
  is defined in [Makefile](../Makefile). Sanitizers use
  [sanitizers.yml](../.github/workflows/sanitizers.yml).
- **Safety / HISS:** bounded dimensions and loops, checked arithmetic and
  returns, initialization-only hot-path allocation, and justified pointer
  operations map to HISS-01/02/03/04/07/08/09/10/15. The
  [catalog](../.config/hiss/coverage.yaml) explicitly leaves recursion and
  pointer proofs unproved; `.clang-tidy` disables `misc-no-recursion`.
  **No gate yet** for complete enforcement of the applicable matrix rules.

### 8.2 C++

- **C core:** Rules 1, 2, 4–8 and 10 apply; rules 3 and 9 use RAII and non-owning views without weakening hot-path allocation or lifetime requirements.
- **Role and standard:** `core/tools/`, host engine `.cpp` files in `core/src/`
  and C++ tests. Follow sections 1–7 with RAII and checked error results;
  keep exceptions out of hot paths. [core/meson.build](../core/meson.build)
  selects `c++26,c++23,c++latest` and probes `std::expected`; C++23 is the
  required library baseline, not a claim that every backend uses that mode.
- **Gates:** the C clang-format, `make lint-c`, Meson test and sanitizer
  commands above also cover configured C++ translation units. The
  [shared-enum contract](../core/test/test_c_cxx_enum_definition_contract.py)
  protects C/C++ header layout. [The CI matrix](../.github/workflows/libvmaf-build-matrix.yml)
  selects fatal-warning modes via [werror-args.sh](../scripts/ci/werror-args.sh);
  the default Meson profile alone does not enforce them.
- **Safety / HISS:** RAII owns resources, C ABI calls validate sizes and
  lifetimes, numerical rewrites preserve reference rounding. HISS-01/02/03/04/07/08/09/10/15
  apply with the adaptations in section 9. **No gate yet** for complete
  call-graph, allocation and pointer-proof checks.

### 8.3 CUDA

- **C core:** Rules 1, 4, 6–8 and 10 apply. Rules 2, 3, 5 and 9 adapt to launch bounds, device memory and host-side invariant checks; device assertions cannot replace input validation.
- **Role and standard:** `core/src/cuda/` and `core/src/feature/cuda/`;
  `.cu` kernels use C++20 in [core/src/meson.build](../core/src/meson.build),
  while C host wrappers retain C rules.
- **Gates:** clang-format hooks include CUDA. `make tidy-lane LANE=cuda`
  ([Makefile](../Makefile)) measures the CUDA lint lane. The
  [build matrix](../.github/workflows/libvmaf-build-matrix.yml) builds configured
  CUDA targets; the Meson test runner above runs CUDA tests registered in
  [core/test/meson.build](../core/test/meson.build) when enabled. Device
  compilation takes `--Werror all-warnings` when Meson `werror` is enabled.
- **Safety / HISS:** bound grid and pixel indices, check driver errors, own
  device buffers and fence lifetimes, allocate outside scoring loops and
  retain the shared strict-FP flags in `core/src/meson.build`.
  HISS-01/02/03/04/07/08/09/10/15 map to host and device code; a host test
  is not device-execution evidence. **No gate yet** for all matrix obligations.

### 8.4 HIP

- **C core:** The CUDA C-core mapping applies to HIP host and device code. A shared algorithm has the same bounds, ownership and numerical contract on both backends.
- **Role and standard:** `core/src/hip/` and `core/src/feature/hip/`;
  host C and `.hip` device C++. [core/src/meson.build](../core/src/meson.build)
  leaves most device targets in hipcc's default mode and explicitly selects
  C++20 for `ciede_score`; there is **no gate yet** requiring one explicit
  language standard for every HIP kernel.
- **Gates:** the `clang-format-hip-metal` hook selects `.hip` by extension;
  `make tidy-lane LANE=hip` is defined in [Makefile](../Makefile).
  [The build matrix](../.github/workflows/libvmaf-build-matrix.yml) and
  [Meson tests](../core/test/meson.build) cover enabled configurations.
  Custom HSACO targets add `-Werror` when `werror` is enabled.
- **Safety / HISS:** check HIP errors, bounds, buffer ownership and acquire/release
  fences; preserve the common no-contraction arithmetic flags.
  HISS-01/02/03/04/07/08/09/10/15 have the CUDA adaptations. **No gate yet**
  for complete static safety coverage; build-only coverage does not prove GPU execution.

### 8.5 SYCL

- **C core:** The CUDA C-core mapping applies to SYCL work-items and host wrappers. Queue/event lifetimes replace implicit sequential completion assumptions.
- **Role and standard:** `core/src/sycl/` and `core/src/feature/sycl/`;
  device translation units use `-std=c++20` in
  [core/src/meson.build](../core/src/meson.build).
- **Gates:** C++ clang-format, `make tidy-lane LANE=sycl`
  ([Makefile](../Makefile)), configured builds and the Meson test runner.
  [sycl-parity.yml](../.github/workflows/sycl-parity.yml) runs the selected
  hardware lane under its runner-enable condition; see
  [cross-backend coverage](development/cross-backend-gate.md).
- **Safety / HISS:** bound work-items, check queue/asynchronous errors,
  pair USM ownership with completion events and enforce import fences.
  Use the common strict-FP policy in `core/src/meson.build`.
  HISS-01/02/03/04/07/08/09/10/15 apply to host and device work; **no gate yet**
  for every bound, lifetime and error path.

### 8.6 Objective-C++ / Metal

- **C core:** The C++ mapping applies to Objective-C++ hosts; the CUDA mapping applies to Metal shaders. ARC does not prove GPU completion or imported-buffer lifetime.
- **Role and standard:** `.mm` host wrappers in `core/src/metal/` and
  `core/src/feature/metal/`, with `.metal` shaders alongside them.
  [core/src/metal/meson.build](../core/src/metal/meson.build) enables ARC,
  disables Objective-C ARC exception unwinding and compiles shaders as Metal 3.1.
  The host target sets no explicit `objcpp_std`; **no gate yet** pins its
  language mode independently of the compiler default.
- **Gates:** `.metal` has the `clang-format-hip-metal` hook; **no gate yet**
  for Objective-C++ formatting (`Language: ObjC` has `DisableFormat: true`
  in [.clang-format](../.clang-format)).
  [tidy-metal.yml](../.github/workflows/tidy-metal.yml) measures host wrappers
  against the Metal baseline; it explicitly excludes shaders because
  clang-tidy has no Metal mode. Shader compilation and
  [registered Metal tests](../core/test/meson.build) use the Meson backend.
  **No gate yet** for shader static lint or shader warnings-as-errors:
  the custom `metal` / `metallib` commands carry no fatal-warning switch.
- **Safety / HISS:** keep ARC/CF ownership explicit, prove texture/IOSurface
  lifetimes and fence ordering, bound shader indices and retain strict FP.
  HISS-01/02/03/04/07/08/09/10/15 cover both halves; host lint cannot stand
  in for shader analysis.

### 8.7 Go

- **C core:** Rules 1, 4, 6 and 10 apply. Rules 2, 3, 5 and 7–9 adapt to bounded services, garbage collection, explicit errors, build tags and unsafe/cgo boundaries.
- **Role and standard:** `cmd/`, `pkg/`, `internal/`, `api/` and Go tools;
  [go.mod](../go.mod) owns the toolchain version. Use idiomatic Go with
  explicit errors, bounded goroutine lifetimes and request cancellation.
- **Gates:** [lefthook.yml](../lefthook.yml) runs `gofmt -w {staged_files}`
  and `go vet ./...`. [go-ci.yml](../.github/workflows/go-ci.yml) runs
  `go fix -diff ./...`, `go vet ./...` and `go test ./...`.
  `make lint-go` runs gosec; `make go-test` selects the in-tree libraries
  ([Makefile](../Makefile)). See the [setup guide](development/languages.md)
  for the cgo and envtest prerequisites.
- **Safety / HISS:** check and wrap errors, propagate `context.Context`
  timeouts, cap queues and workers, document `unsafe`/cgo ownership with
  `SAFETY:` proofs. HISS-01/02/03/04/07/08/09/10/15 are adapted to Go.
  The [catalog](../.config/hiss/coverage.yaml) leaves method cycles and bare
  discarded calls as gaps. **No gate yet** for a non-mutating Go format
  rejection or full resource/error coverage.

### 8.8 Rust

- **C core:** Rules 1, 2, 4, 6 and 10 apply. Rules 3, 5 and 7–9 adapt to ownership, Result, macros and unsafe/FFI contracts; borrowing does not permit allocation in metric loops.
- **Role and standard:** [Cargo.toml](../Cargo.toml) owns the FFI binding
  workspace under `bindings/rust/`; [core/src/rust/Cargo.toml](../core/src/rust/Cargo.toml)
  owns the linked extractor/predictor workspace, including
  `core/src/feature/rust/tad/`. Both declare edition 2024 and follow
  [rust-toolchain.toml](../rust-toolchain.toml), not an unpinned stable release.
- **Gates:** [rust-ci.yml](../.github/workflows/rust-ci.yml) runs
  `cargo fmt --all --check`, `cargo clippy --workspace --all-targets -- -D warnings`
  and `cargo test --workspace --all-features` at the root, plus
  `cargo fmt --manifest-path core/src/rust/Cargo.toml --all --check`,
  `cargo clippy --manifest-path core/src/rust/Cargo.toml --workspace --all-targets -- -D warnings`
  and `cargo test --manifest-path core/src/rust/Cargo.toml --workspace`.
  It sets `RUSTFLAGS: -D warnings`. [clippy.toml](../clippy.toml) supplies
  length and cognitive-complexity thresholds for the core workspace lints.
- **Safety / HISS:** `Result` propagation, no `unwrap`/`expect`,
  `SAFETY:` proofs and safety documentation at unsafe boundaries, no unwinding
  into C, no allocation in metric loops. These map to
  HISS-01/02/03/04/07/08/09/10/15. Core Clippy denies several of them, but
  the [catalog](../.config/hiss/coverage.yaml) and differing workspace lints
  leave gaps; **no gate yet** for complete recursion, allocation, complexity
  (McCabe is not cognitive complexity) and unsafe-trait coverage.

### 8.9 Python

- **C core:** Rules 1, 4, 6 and 10 apply. Rules 2, 3, 5, 7 and 9 adapt to bounded collections, resource contexts, exceptions and ctypes; rule 8 has no C preprocessor to govern.
- **Role and standard:** training in `ai/`, legacy harness in `python/`
  and `compat/python-vmaf/`, `mcp-server/vmaf-mcp/`, `scripts/`, `tools/`
  and generated `bindings/python/`. Package `pyproject.toml` files own
  interpreter floors; [root pyproject.toml](../pyproject.toml) owns Black,
  Ruff, mypy and pytest settings. Follow PEP 8 and typed boundaries; Python
  does not implement hot-path scoring (the role retained from ADR-0702).
- **Gates:** Black and Ruff hooks in [.pre-commit-config.yaml](../.pre-commit-config.yaml),
  `make lint-py` and `make format-check` in [Makefile](../Makefile).
  Mypy is strict by configuration but advisory in `make lint-py`; the
  [mypy delta hook](../scripts/git-hooks/pre-push-mypy.py) has its own scope.
  [The test workflow](../.github/workflows/tests-and-quality-gates.yml)
  runs package suites; [the test guide](development/test-suites.md) owns the
  suite map. Pytest uses `filterwarnings = ["error"]` in `pyproject.toml`.
- **Safety / HISS:** bound datasets, subprocesses and I/O, validate untrusted
  inputs, propagate exceptions with context, close resources, prohibit
  `eval`/`exec`, and explain ctypes memory ownership. HISS-01/02/03/04/07/08/09/10/15
  are adapted to managed code. Ruff's exceptions and the
  [partial HISS matchers](../.config/hiss/coverage.yaml) are not full coverage;
  **no gate yet** for all matrix obligations or a fully blocking type check.

### 8.10 Shell

- **C core:** Rules 1, 4 and 10 apply. Rules 2, 3 and 5–7 adapt to process budgets, bounded work and checked statuses; rules 8 and 9 have no C preprocessor or pointers to govern.
- **Role and standard:** `.sh` automation in `scripts/`, `dev/` and hooks.
  Follow each shebang's dialect; Bash scripts use `set -euo pipefail`
  (section 1.5), while `/bin/sh` scripts must stay POSIX-compatible.
- **Gates:** shfmt (`-i 2 -ci`) and shellcheck hooks in
  [.pre-commit-config.yaml](../.pre-commit-config.yaml);
  `make format-check` and `make lint-sh` in [Makefile](../Makefile).
  The latter also invokes named shell/Python regression scripts. There is
  **no gate yet** that runs every shell script's behavior tests.
- **Safety / HISS:** quote expansions, avoid `eval`, pass arguments without
  word splitting, check pipeline/command failures and bound retries/timeouts.
  Do not treat `set -e` alone as complete error handling.
  HISS-01/02/03/04/07/08/10/15 adapt to process control; HISS-09 applies
  at the native code boundary rather than to shell variables.

### 8.11 MATLAB

- **C core:** Rules 1, 4 and 10 apply. Rules 2, 3 and 5–7 adapt to array bounds, preallocation and checked file operations; rules 8 and 9 attach to MEX C rather than .m source.
- **Role and standard:** inherited reference metrics in
  `compat/python-vmaf/matlab/` (`.m`, alongside C MEX code). Preserve
  upstream numerical behavior and licensing; this tree declares **no
  pinned MATLAB language version**. MEX C follows section 8.1.
- **Gates:** **no gate yet** for `.m` format, lint, runtime tests or
  warnings-as-errors. [The native lint workflow](../.github/workflows/lint-and-format.yml)
  parses MEX C with [MATLAB stubs](../scripts/ci/lint-stubs/matlab/);
  that is not MATLAB language coverage.
- **Safety / HISS:** validate array shapes, bound frame loops, preallocate
  numeric workspaces and check file operations; preserve reference rounding.
  HISS-01/02/03/04/07/08/10/15 adapt to array code; raw-pointer proof
  obligations belong to MEX C (HISS-09).

### 8.12 YAML, Helm templates and GitHub Actions

- **C core:** Rules 2, 5, 7 and 10 adapt to bounded jobs, configuration validation and failing steps. C flow, heap, scope, macro and pointer rules have no direct YAML equivalent; embedded programs inherit their own rules.
- **Role and standard:** configuration in `.standards.yaml`, `mkdocs.yml`,
  `deploy/`, `config/`, `dev/docker-compose.yml`, workflows and composite
  actions in `.github/actions/`. Use each consumer's pinned schema;
  Helm templates are rendered before YAML validation, and multi-document
  Kubernetes manifests must retain document boundaries. Workflow schemas are selected by
  [.pre-commit-config.yaml](../.pre-commit-config.yaml) and runner labels in
  [.github/actionlint.yaml](../.github/actionlint.yaml).
- **Gates:** `make lint-actions` ([Makefile](../Makefile)) runs
  [run_actionlint.py](../scripts/ci/run_actionlint.py),
  [check_composite_actions.py](../scripts/ci/check_composite_actions.py) and
  the declared runner, workflow-budget and timeout checks.
  Hooks also run `check-yaml` and `check-github-actions`, with exclusions
  recorded in their configuration. `helm lint deploy/helm/vmafx` and
  `helm template vmafx deploy/helm/vmafx` cover the chart defaults in
  [helm-chart.yml](../.github/workflows/helm-chart.yml); optional values
  require their corresponding rendered cases. Neither actionlint nor Helm
  validates arbitrary YAML for every consumer. **No gate yet**
  for YAML formatting or complete behavioral coverage of workflow paths.
- **Safety / HISS:** pin actions, minimize permissions, pass untrusted
  expressions through environment values rather than script interpolation,
  and bound jobs/steps. Reject duplicate keys and schema-invalid values;
  configuration must not execute untrusted constructors. HISS-02/07/08/10/15
  adapt to orchestration and consumer validation;
  embedded scripts additionally follow their own language section.

### 8.13 TOML configuration and API definitions

- **C core:** Rules 5 and 10 adapt to schema and consumer validation. Other C-core rules have no executable TOML construct; ABI error and pointer obligations apply to generated consumers.
- **Role and standard:** Cargo manifests, `rust-toolchain.toml`,
  `pyproject.toml`, `REUSE.toml`, tool configuration and API definitions.
  Each consumer owns its schema; valid TOML syntax alone does not establish
  valid Cargo, Python, licence or tooling configuration.
  [core/api/vmafx.toml](../core/api/vmafx.toml) is the
  API definition, not executable code; its `schema = 2` contract is checked
  by [validate.py](../scripts/codegen/vmafx_api/validate.py).
- **Gates:** `check-toml` in [.pre-commit-config.yaml](../.pre-commit-config.yaml)
  checks syntax. `python3 scripts/codegen/vmafx-api.py --check` validates
  generated outputs; `python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master`
  checks compatibility. [core/test/meson.build](../core/test/meson.build)
  registers generated-current, append-only and generator tests.
  [api/vmafx-platform.toml](../api/vmafx-platform.toml) owns the platform
  definition checked by the same generator. Cargo and Python consumer
  checks follow their language sections; no single schema gate covers all TOML.
  [The generation guide](development/api-generation.md) owns the API schema and
  update procedure. **No gate yet** for TOML formatting.
- **Safety / HISS:** keep types, sizes, ownership, error codes and versioning
  explicit; never hand-edit generated outputs. HISS-07/09/15 adapt to
  validating the described ABI, HISS-08 to declarative input and HISS-10 to
  failing validation. HISS-14's append-only contract remains mandatory.

### 8.14 Meson

- **C core:** Rules 1, 2, 5–8 and 10 adapt to an acyclic build graph, bounded configuration work, scoped variables, explicit dependencies and checked commands. Kernel allocation, function-size and pointer rules belong to compiled source.
- **Role and standard:** `core/meson.build`, `core/meson_options.txt`,
  `core/**/meson.build` and `gstreamer/meson.build`; use the version pinned
  in [requirements/locks/build.in](../requirements/locks/build.in).
- **Gates:** `meson setup build core`, `ninja -C build` and the Meson test
  runner validate configured paths; [the build matrix](../.github/workflows/libvmaf-build-matrix.yml)
  exercises platform/backend variants. **No gate yet** for a Meson-specific
  formatter/linter; compilation does not evaluate every option branch.
- **Safety / HISS:** declare dependencies and generated inputs, check
  subprocess results deliberately and keep toolchains/FP flags shared.
  HISS-02/07/08/10/15 adapt to bounded configuration and validation;
  algorithmic and pointer rules belong to generated/compiled source.

### 8.15 Containerfiles

- **C core:** Rules 2, 5, 7 and 10 adapt to bounded build steps, smoke validation and propagated failures. Other C-core constructs belong to embedded shell and compiled programs.
- **Role and standard:** `dev/Containerfile`, `Containerfile.vmafx`, root
  `Dockerfile*`, `docker/`, `.devcontainer/Dockerfile.praetor` and
  `mcp-server/vmaf-mcp/Dockerfile`; use each recipe's declared Dockerfile
  frontend (`# syntax=...`), including its version and digest where present.
  [dev/Containerfile](../dev/Containerfile) and [Containerfile.vmafx](../Containerfile.vmafx)
  declare their own frontend pins; the
  [container workflows](../.github/workflows/dev-container-build.yml) select builders.
- **Gates:** [check-container-image-references.py](../scripts/ci/check-container-image-references.py)
  and [check-base-image-single-source.sh](../scripts/ci/check-base-image-single-source.sh)
  check image inputs; their hooks live in [.pre-commit-config.yaml](../.pre-commit-config.yaml).
  Builds in [dev-container-build.yml](../.github/workflows/dev-container-build.yml)
  and [supply-chain.yml](../.github/workflows/supply-chain.yml) cover their
  selected recipes. **No gate yet** for Containerfile formatting, general
  recipe lint (no hadolint gate), or one test suite covering every recipe.
- **Safety / HISS:** pin image/dependency inputs, isolate build secrets,
  check `RUN` failures and retain runtime smoke coverage. HISS-07/08/10/15
  adapt to build steps; HISS-11 governs provenance/pinning. Embedded shell
  follows section 8.10; a successful image build is not a zero-warning gate.

### 8.16 TypeScript / JavaScript

- **C core:** Rules 1, 4 and 10 apply. Rules 2, 3 and 5–7 adapt to bounded DOM/data work, managed memory, promises and module scope. Rules 8 and 9 have no C preprocessor or raw pointers to govern.
- **Role and standard:** tracked figure specs in `docs/figures/*.ts`, the
  engine and player in `tools/figures/`, documentation scripts in
  `docs/javascripts/`, math checks in `scripts/docs/check_math_worker.js`
  and Markdown tooling in `tools/markdownlint/`. Retain vendored notices;
  use modules and the Node version of [praetor-docs.yml](../.github/workflows/praetor-docs.yml)
  for its tooling, browser-compatible code for the player/site.
- **Gates:** `make docs-figures` runs `node tools/figures/build.mjs check`
  and `node tools/figures/build.mjs sources`; `make docs-math-check` runs
  the math worker through [check_math.py](../scripts/docs/check_math.py).
  [check_vendored_assets.py](../scripts/docs/check_vendored_assets.py) verifies
  committed vendor assets. [eslint.figures.mjs](../tools/figures/eslint.figures.mjs)
  is an adopter configuration helper, not a repository lint invocation.
  **No gate yet** for tree-wide JS/TS format, lint, static type checking,
  warnings-as-errors or a unified runtime test gate.
- **Safety / HISS:** bound input/DOM work and async waits, handle rejected
  promises, avoid dynamic evaluation and validate renderer inputs.
  HISS-01/02/03/04/07/08/10/15 adapt to managed code; native ownership stays
  at the FFI boundary (HISS-09).

### 8.17 Assembly (NASM)

- **C core:** rules 1, 2 and 4–10 adapt to structured branches, bounded
  register/index use, short routines, ABI invariants, explicit clobbers and
  inspectable assembler macros. Rule 3 still prohibits hot-path allocation.
- **Additions:** [cpuid.asm](../core/src/x86/cpuid.asm) and the inherited
  [x86inc.asm](../core/src/ext/x86/x86inc.asm) must preserve calling
  conventions, stack alignment, callee-saved registers and feature guards.
  Record bounds and alignment proofs for memory operands; retain licences.
- **Gate command:** `ninja -C build` assembles the configured x86 path through
  [core/src/meson.build](../core/src/meson.build). This is not assembly lint
  or instruction-level conformance evidence. **No gate yet** for a complete
  assembly safety/format policy; changed dispatch needs execution evidence.

### 8.18 SQL

- **C core:** rules 1, 2, 5, 7 and 10 adapt to bounded queries, constraints,
  transactions and checked database errors. C heap, function length, scope,
  preprocessor and pointer restrictions do not describe declarative SQL.
- **Additions:** controller SQL in `cmd/vmafx-controller/queue/` and
  `cmd/vmafx-controller/store/` must parameterize values, bound result sets
  and lock waits, preserve migration order and test rollback behavior.
  Do not interpolate user input into queries. Callers own cancellation and
  transaction cleanup; a generated query is not proof of query cost.
- **Gate command:** `python3 scripts/codegen/sqlc_generate.py --check`
  checks generated PostgreSQL query bindings against the
  [sqlc configuration](../cmd/vmafx-controller/store/sqlc.yaml).
  Exit 77 means unavailable, not passing. It does not cover queue SQLite SQL,
  live migrations or every isolation failure. **No gate yet** for all SQL
  formatting, migration and resource-bound obligations.

### 8.19 Protobuf

- **C core:** rules 5 and 10 adapt to schema/compatibility validation;
  runtime flow, heap, scope, errors, preprocessor and pointer rules apply to
  generated consumers rather than message declarations.
- **Additions:** `proto/` is generated from the TOML definitions. Never
  renumber fields or reuse removed field numbers; preserve wire and JSON
  compatibility, and bound payloads at service entry points. Edit the
  definition and regenerate, not the `.proto` or `.pb.go` output.
- **Gate command:**
  `python3 scripts/codegen/proto_generate.py --check --breaking-against origin/master`
  runs the pinned Buf lint, generation-drift and wire/JSON compatibility
  checks through [the wrapper](../scripts/codegen/proto_generate.py) and
  [buf.yaml](../buf.yaml). Exit 77 means unavailable. Generated-code checks
  do not prove runtime payload limits; **no gate yet** for every obligation.

### 8.20 PowerShell

- **C core:** rules 1, 4 and 10 apply. Rules 2, 3 and 5–7 adapt to bounded
  processes, scoped state and checked exceptions/exit codes. Rules 8 and 9
  have no C preprocessor or raw pointers to govern.
- **Additions:** `.ps1` setup and CI scripts use explicit parameters and
  argument arrays, stop on cmdlet failures, check native `$LASTEXITCODE`,
  verify downloaded artifact digests and bound retries. Avoid
  `Invoke-Expression`; `$ErrorActionPreference = 'Stop'` alone does not
  check native program failures. Windows workflow blocks follow this policy.
- **Gate command:**
  `python3 -m pytest scripts/ci/tests/test_setup_zimg_windows.py` checks the
  zimg workflow-call contract only. Windows build workflows exercise selected
  installers. **No gate yet** for general PowerShell lint, formatting or
  full script behavior; text-contract tests are not Windows execution proof.

### 8.21 Windows batch

- **C core:** rules 1, 4 and 10 apply; rules 2, 3 and 5–7 adapt as for shell.
  C macro and pointer restrictions have no direct counterpart. The ban on
  `goto` still applies, even though the dialect provides it.
- **Additions:** [run.cmd](../tools/rc1-tester/image/windows/run.cmd) scopes
  variables with `setlocal`, quotes paths and propagates the child exit code.
  Preserve arguments across spaces and metacharacters; bound child runtime
  in the caller and do not treat launcher success as report success.
- **Gate command:** **no gate yet** for a standalone `.cmd` lint/format or
  behavior suite. The Windows tester bundle workflow is scoped packaging
  evidence, not a general batch gate.

### 8.22 Lua

- **C core:** rules 1, 4 and 10 apply; rules 2, 3 and 5–7 adapt to bounded
  editor work, managed memory, checked calls and local scope. Rules 8 and 9
  have no C preprocessor or raw pointers to govern.
- **Additions:** [.nvim.lua](../.nvim.lua) and
  [lua/standards.lua](../lua/standards.lua) configure editor integration.
  Keep module-loading and process-launch paths explicit, report failed
  protected calls and avoid evaluating untrusted Lua source.
- **Gate command:** **no gate yet** for Lua formatting, lint or editor-runtime
  tests. An editor command that invokes a repository gate does not validate
  the Lua integration itself.

### 8.23 Make

- **C core:** rules 1, 2, 5–8 and 10 adapt to an acyclic dependency graph,
  bounded recipes, scoped settings, inspectable expansion and propagated
  failures. Kernel allocation, function-size and pointer rules belong to
  the programs each recipe builds or launches.
- **Additions:** [Makefile](../Makefile) and included `.mk` files declare
  prerequisites and generated outputs. Share tool/version settings, quote
  paths and keep shell arguments intact. A leading `-`, `|| true` or a later
  successful command must not hide a required failure. Recipe shell follows
  section 8.10; expansion-time commands require the same scrutiny.
- **Gate command:** `make lint-c` exercises its configured native recipe;
  use the target owning the changed behavior for other recipes. **No gate
  yet** for Make-specific lint/format or every target/variable combination.
  A dry run can evaluate Make expansion-time commands and is not a sandbox.

### 8.24 CMake package templates

- **C core:** rules 1, 2, 5–8 and 10 adapt as for Meson. There is no scoring
  loop or raw pointer in the package-definition language.
- **Additions:** [vmafxConfig.cmake.in](../core/cmake/vmafxConfig.cmake.in)
  is configured by Meson. Preserve relocatable install paths, imported
  target properties, platform library naming and caller variable state.
  Validate the intended shared-library lookup; do not silently select an
  unrelated system installation.
- **Gate command:** `ninja -C build` covers template generation when the
  [Meson CMake-package branch](../core/src/meson.build) is enabled. **No gate
  yet** for complete CMake format/lint and installed-consumer validation;
  generating the template does not prove `find_package` behavior.

### 8.25 CSS

- **C core:** rules 5 and 10 adapt to rendered-state validation and diagnostic
  hygiene; procedural control flow, heap, function, scope, return, macro and
  pointer restrictions have no direct CSS equivalent.
- **Additions:** `docs/stylesheets/`, `tools/figures/figures.css` and vendored
  styles must preserve keyboard focus, contrast, reduced-motion behavior
  and responsive layouts. Keep selectors scoped and retain vendor notices.
- **Gate command:** `python3 scripts/docs/check_vendored_assets.py` checks
  integrity of assets declared in its manifest, not authored CSS semantics.
  **No gate yet** for general CSS lint/format or browser visual/accessibility
  regression coverage.

### 8.26 HTML

- **C core:** rules 5 and 10 adapt to markup/consumer validation. Other C-core
  constructs belong to embedded JavaScript or native consumers.
- **Additions:** the tracked HTML fixture in
  `pkg/encodeprofile/testdata/profile.html` and generated documentation need
  escaped untrusted content, semantic structure and accessible controls.
  Preserve fixture intent; generated HTML is changed through its source.
- **Gate command:** `node tools/figures/build.mjs sources` checks the figure
  markup contract only. **No gate yet** for tree-wide HTML validation or
  browser behavior. Embedded scripts and styles also follow their sections.

### 8.27 JSON

- **C core:** rules 5 and 10 adapt to syntax/schema validation. Other C-core
  constructs belong to consumers; a JSON string must remain data.
- **Additions:** configuration, schemas, models and snapshots retain explicit
  versions, numeric precision and deterministic generation. Reject duplicate
  keys and unbounded nesting at input boundaries. Intentionally malformed
  fuzz fixtures remain byte-exact; do not format model binaries as text.
- **Gate command:** `python3 scripts/docs/check_vendored_assets.py` checks its
  declared asset manifest and schema files. The `check-json` hook in
  [.pre-commit-config.yaml](../.pre-commit-config.yaml) adds syntax coverage
  with named exclusions. **No gate yet** for every JSON consumer schema,
  formatting rule or behavioral contract; syntax acceptance is insufficient.

### 8.28 INI / Meson cross files

- **C core:** rules 5 and 10 adapt to consumer validation; C algorithm, heap,
  scope, error-return, macro and pointer syntax is absent from INI data.
- **Additions:** `build-aux/*.ini` declares cross compilers, executors and
  host properties. Keep architecture/ABI, toolchain and strict-FP assumptions
  consistent with the corresponding backend. Executable entries inherit the
  called tool's bounds and failure policy.
- **Gate command:**
  `meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini`
  validates that cross profile during configuration, as documented in
  [Makefile](../Makefile). Compilation and target execution remain separate
  evidence. **No gate yet** for all INI consumers, formatting or every cross profile.

### 8.29 Markdown / reStructuredText

- **C core:** rules 5 and 10 adapt to reference, rendering and diagnostic
  validation. Other C-core rules apply to executable examples, not prose.
- **Additions:** `docs/`, root Markdown and [python/README.rst](../python/README.rst)
  document behavior with reproducible commands and tracked evidence links.
  Keep examples consistent with their implementation language; rendered
  tables, equations and diagrams must remain usable without script execution.
- **Gate command:** `node tools/markdownlint/verify.mjs` checks the declared
  Markdown corpus; `mkdocs build --strict` validates the configured site.
  [The documentation workflow](../.github/workflows/praetor-docs.yml) owns
  the former command. **No gate yet** for general reStructuredText rendering,
  all example execution or every accessibility requirement.

### 8.30 Cross-language interface policy (ADR-0702, updated by ADR-1852)

The engine's language-neutral boundary is the
[VMAFx C API](api/vmafx/index.md): `core/include/vmafx/`, `libvmafx.so.1`.
The separate `libvmaf.so.3` compatibility library preserves the
[libvmaf interface](api/index.md), rather than making its headers the sole
interface for new bindings. SONAMEs and the preview ABI version are separate;
[ADR-1852](adr/1852-vmafx-api-redesign.md) and
[the API generation guide](development/api-generation.md) define that contract.

`core/api/vmafx.toml` owns generated surfaces and bindings; modify the
definition and regenerate, never fork the ABI in a language wrapper. The
[API guide](api/vmafx/index.md) identifies delivered preview surfaces;
[the Rust binding guide](api/rust-context-close.md) describes the existing
libvmaf-backed wrapper, so the RC4 generation policy is not a claim that
all legacy bindings have migrated. Languages consume the C ABI rather than
calling one another directly. New language adoption requires an ADR,
compile/validation coverage and fast tests. Production tooling binaries do
not embed training code or model weights; training stays in `ai/`.

## 9. Applicability matrix

This table maps the ten C-core rules in section 1.1 to every language above.
It records **policy applicability**, not enforcement or a passing result.
**A** = applies directly; **D** = adapted to the language as explained in
section 8; **—** = the C construct is absent from this surface. An absent
construct does not exempt embedded programs, generated code or native FFI.
The inherited JPL/CERT constraints still govern native operations.

| Language / surface | 1 Flow | 2 Bounds | 3 Allocation | 4 Size | 5 Invariants | 6 Scope | 7 Returns | 8 Preprocessor | 9 Pointers | 10 Diagnostics |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C | A | A | A | A | A | A | A | A | A | A |
| C++ | A | A | D | A | A | A | A | A | D | A |
| CUDA | A | D | D | A | D | A | A | A | D | A |
| HIP | A | D | D | A | D | A | A | A | D | A |
| SYCL | A | D | D | A | D | A | A | A | D | A |
| Objective-C++ host | A | A | D | A | A | A | A | A | D | A |
| Metal shaders | A | D | D | A | D | A | A | A | D | A |
| Go | A | D | D | A | D | A | D | D | D | A |
| Rust | A | A | D | A | D | A | D | D | D | A |
| Python | A | D | D | A | D | A | D | — | D | A |
| Shell | A | D | D | A | D | D | D | — | — | A |
| MATLAB | A | D | D | A | D | D | D | — | — | A |
| YAML / Helm / Actions | — | D | — | — | D | — | D | — | — | D |
| TOML | — | — | — | — | D | — | — | — | — | D |
| Meson | D | D | — | — | D | D | D | D | — | D |
| Container recipes | — | D | — | — | D | — | D | — | — | D |
| TypeScript | A | D | D | A | D | D | D | — | — | A |
| JavaScript | A | D | D | A | D | D | D | — | — | A |
| Assembly | D | D | A | D | D | D | D | D | D | D |
| SQL | D | D | — | — | D | — | D | — | — | D |
| Protobuf | — | — | — | — | D | — | — | — | — | D |
| PowerShell | A | D | D | A | D | D | D | — | — | A |
| Windows batch | A | D | D | A | D | D | D | — | — | A |
| Lua | A | D | D | A | D | D | D | — | — | A |
| Make | D | D | — | — | D | D | D | D | — | D |
| CMake templates | D | D | — | — | D | D | D | D | — | D |
| CSS | — | — | — | — | D | — | — | — | — | D |
| HTML | — | — | — | — | D | — | — | — | — | D |
| JSON | — | — | — | — | D | — | — | — | — | D |
| INI | — | — | — | — | D | — | — | — | — | D |
| Markdown | — | — | — | — | D | — | — | — | — | D |
| reStructuredText | — | — | — | — | D | — | — | — | — | D |

Adaptations preserve the following obligations:

- **Flow and bounds (1–2):** no recursion or `goto` in executable source;
  bounded work, retries, queue depth and I/O lifetime. Assembly uses
  structured, inspectable branch graphs; build dependencies remain acyclic.
  SQL avoids recursive query paths and bounds result sets and lock waits.
  Recipes bound child processes; schemas declare limits enforced by consumers.
- **Allocation and size (3–4):** no heap allocation in scoring loops,
  including hidden C++ containers and Rust collections. Managed services
  and scripts bound retained data, workers and subprocesses; they do not
  acquire a blanket allocation ban during setup or training. Executable
  functions retain the 60-line bound; configuration is split by responsibility
  rather than pretending every table or build target is a C function.
- **Invariants and scope (5–6):** native code keeps the assertion-density
  requirement. Device code validates launch invariants on the host and uses
  supported device checks. Managed languages use explicit contracts and
  validation; input rejection must survive disabled debug assertions.
  Data formats validate accepted, rejected and boundary configurations.
  Keep bindings local to a block, module, script or build target as supported.
- **Errors (7):** handle error-bearing results, exceptions, rejected promises,
  asynchronous backend errors and process statuses. Ignoring a pure value
  is different from discarding an error. Propagate failures through workflow,
  recipe and container layers; `set -e` alone is insufficient.
- **Metaprogramming and pointers (8–9):** keep macros, build tags, generation
  and substitutions explicit and inspectable. Safe references do not inherit
  C's single-dereference syntax restriction. Native/FFI/unsafe boundaries
  still require bounds, lifetime, ownership and aliasing proofs. Bindings
  and schemas describe the same ABI rather than creating a second one.
- **Diagnostics (10):** compiler, linter, formatter, validator and builder
  findings must fail the relevant gate. A debt ratchet, syntax parser or
  advisory check is partial coverage, not proof of zero warnings.

The HISS contract follows the same mapping: HISS-01/02/03 correspond to
flow, bounds/I/O and hot-path allocation; HISS-04 adds McCabe complexity
≤ 10 and ≤ 50 statements to the 60-line limit. HISS-07 requires handled
errors; HISS-08 prohibits dynamic evaluation and unsafe library calls;
HISS-09 requires `SAFETY:` proofs at unsafe and pointer-arithmetic boundaries.
A language without a C preprocessor is still subject to the HISS-08 ban
on `eval`, `exec` and equivalent evaluation of untrusted source. HISS-10
requires diagnostic hygiene and HISS-15 requires positive, negative and
boundary tests for public interfaces, including generated configurations.
HISS-11 pinning/provenance and HISS-14 append-only ABI obligations apply
across languages; HISS-16–21 remain repository-wide obligations.

### 9.1 Missing gates and partial coverage

Every language has incomplete enforcement of at least one applicable rule.
The commands in section 8 establish only the scope named beside them.
**No gate yet** means missing or incomplete rejection, not a policy waiver.

| Language / surface | Available rejection mechanism | Missing or partial coverage |
| --- | --- | --- |
| C / C++ | Configured clang-tidy/cppcheck, clang-format, compiler profiles, sanitizers and Meson tests | Complete recursion, allocation, pointer proofs, assertion density and HISS-04 limits |
| CUDA / HIP / SYCL | Backend compilation, scoped tidy lanes and registered tests | Complete device analysis, asynchronous lifetimes and hardware execution in every configuration |
| Objective-C++ / Metal | ARC host build, host tidy baseline, shader compilation and registered tests | Objective-C++ formatting, shader static lint and fatal shader warnings |
| Go | gofmt hook, vet, gosec and package tests | Non-mutating format rejection, method cycles, discarded calls and complete resource bounds |
| Rust | Both workspaces' fmt, Clippy and tests; fatal warnings in CI | Complete recursion/allocation/unsafe-trait analysis and equivalent lints across both workspaces |
| Python | Black, Ruff and selected package tests | Blocking whole-scope type checks and complete resource/FFI/error coverage |
| Shell | ShellCheck, shfmt and named regression scripts | Every script's behavior, bound and failure path |
| MATLAB | MEX C lint with stubs | All .m lint, format, runtime and warnings-as-errors gates |
| YAML / Helm / Actions | Selected syntax/schema checks, actionlint, chart lint/render and workflow contracts | Every YAML consumer, formatting and all rendered configuration branches |
| TOML | Syntax hook, API generator and language-specific consumers | General formatting and every consumer schema; syntax is not ABI validation |
| Meson | Configuration, compilation and registered tests | Dedicated lint/format and every option/platform branch |
| Container recipes | Image-reference checks and selected image builds | General recipe lint/format, fatal diagnostics and every runtime smoke case |
| TypeScript / JavaScript | Figure, math and vendor-asset checks | Tree-wide lint, format, static types, fatal warnings and browser runtime tests |
| Assembly | NASM through the configured native build | Dedicated lint/format, memory proofs and full ABI/instruction conformance |
| SQL | sqlc generation drift for configured PostgreSQL queries | SQLite coverage, full migrations, isolation, resource limits and SQL formatting |
| Protobuf | Buf lint, generation drift and wire/JSON compatibility | Runtime payload limits and complete consumer behavior |
| PowerShell | Selected Windows builds and workflow-call contracts | General lint/format, native exit handling and every installer path |
| Windows batch / Lua | No standalone language gate declared | General lint/format and behavior tests |
| Make / CMake templates | Selected recipe execution and package-template generation | Dedicated lint/format and every recipe or installed-consumer branch |
| CSS / HTML | Selected asset integrity and figure-markup checks | General lint/format, browser behavior and accessibility tests |
| JSON / INI | Selected syntax, manifest and consumer checks | Every consumer schema, formatting and boundary behavior |
| Markdown / reStructuredText | Markdown gate and configured MkDocs site | General reStructuredText rendering, executable examples and full accessibility |

Do not promote these partial mechanisms to complete gates:

- [The HISS catalog](../.config/hiss/coverage.yaml) declares selected C,
  Go, Python and Rust matchers. It leaves method cycles, several discarded
  errors, dynamic imports, unsafe traits and native pointer proofs as gaps;
  it makes no complete per-language claim for GPU sources or recipes.
- [.clang-tidy](../.clang-tidy) checks 60-line functions but sets
  `StatementThreshold: 120` and `BranchThreshold: 15`; that is not the
  HISS-04 cap of 50 statements and McCabe complexity 10. Rust's
  [cognitive-complexity threshold](../clippy.toml) is a different metric.
- `make assertion-density` ([assertion-density.sh](../scripts/ci/assertion-density.sh))
  checks selected fork-authored C/C++ functions for at least one assertion
  when at least 20 lines long; the two-assertion average is informational.
  It does not enforce whole-tree Power-of-10 rule 5.
- [werror-args.sh](../scripts/ci/werror-args.sh) enables fatal compiler/linker
  diagnostics only in selected profiles; custom Metal shader commands do
  not inherit them. Go formatting mutates staged files and mypy's Make
  target is advisory. Neither proves complete HISS-10 coverage.
- No universal gate mandates all three HISS-15 cases for every interface.
  A parser, successful build, skip or host-only test cannot supply missing
  behavioral, negative-case or device evidence.

The enforcement slice of [#2438](https://github.com/VMAFx/vmafx/issues/2438)
must fill missing checks and demonstrate rejection with planted defects.
This document adds no scanner or exception. A new check needs a planted
negative case that fails without the fix, plus accepted and boundary cases.
If a scanner imposes a rule where its C construct is absent, report the
mismatch with the matrix cell and a reproducer; do not silently weaken the
gate or change applicability to hide a finding.

## 10. Revising this document

Changes to this document require:

1. A dedicated PR titled `docs(principles): <summary>`.
2. Maintainer review and the required checks on the exact head through the
   repository's merge process. An independent second human approval is not
   required while there is one maintainer; the bounded exceptions in
   [ADR-2126](adr/2126-scorecard-single-maintainer-exceptions.md) cover that
   limitation without changing protection settings. Revisit them at their
   expiry, `v1.0.0` or when a second maintainer joins.
3. Tooling changes when adding enforcement, with positive, negative and
   boundary evidence. A documentation-only slice must identify missing
   gates explicitly and link the follow-up; a policy statement is not a
   passing test.
4. Linked rationale: a design record, security incident or standards update.
