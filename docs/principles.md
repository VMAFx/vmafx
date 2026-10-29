<!-- markdownlint-disable MD013 MD060 -->
# VMAF Fork — Engineering Principles

This page defines the non-negotiable standards for code merged to `master`.
Read it before changing code, scripts or build definitions.

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
by the Netflix golden assertions (section 8 of
[AGENTS.md](https://github.com/VMAFx/vmafx/blob/master/AGENTS.md), and
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
  gate and as a pre-commit hook; bandit runs through the Python lint
  configuration.
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
`cebdcbcd585892fceabc3235e3dd4fee8dbf43d8`, checked on 2026-10-28 with
`git ls-files`. TypeScript and JavaScript are present; future languages do
not acquire a policy or a claimed gate until their source arrives.
[ADR-3385](adr/3385-principles-per-language-matrix.md) records this docs-only
slice of [#2438](https://github.com/VMAFx/vmafx/issues/2438).

**Common contract.** HISS-10 requires compiler, linter and formatter
findings to fail in every language; a ratchet, advisory check or parser is
not proof of that complete policy. HISS-15 requires positive, negative and
boundary cases for public interfaces. Section 9 marks missing enforcement
with **† (no gate yet)**, including rules with only partial checks. The
[HISS evidence catalog](../.config/hiss/coverage.yaml) limits what
`praetorctl hiss coverage --verify` proves; replaying that catalog does not
prove every rule in every language. Existing exceptions remain explicit in
[the standards manifest](../.standards.yaml) and
[declared lint exceptions](../.config/lint-exceptions.d/ruff.toml).

### 8.1 C

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
  **No gate yet** for full coverage of the marked matrix cells.

### 8.2 C++

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
- **Safety / HISS:** `Result` propagation, no production `unwrap`/`expect`,
  `SAFETY:` proofs and safety documentation at unsafe boundaries, no unwinding
  into C, no allocation in metric loops. These map to
  HISS-01/02/03/04/07/08/09/10/15. Core Clippy denies several of them, but
  the [catalog](../.config/hiss/coverage.yaml) and differing workspace lints
  leave gaps; **no gate yet** for complete recursion, allocation, complexity
  (McCabe is not cognitive complexity) and unsafe-trait coverage.

### 8.9 Python

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

### 8.12 GitHub Actions YAML

- **Role and standard:** workflows in `.github/workflows/` and composite
  actions in `.github/actions/`; use the GitHub Actions schemas selected by
  [.pre-commit-config.yaml](../.pre-commit-config.yaml) and runner labels in
  [.github/actionlint.yaml](../.github/actionlint.yaml).
- **Gates:** `make lint-actions` ([Makefile](../Makefile)) runs
  [run_actionlint.py](../scripts/ci/run_actionlint.py),
  [check_composite_actions.py](../scripts/ci/check_composite_actions.py) and
  the declared runner, workflow-budget and timeout checks.
  Hooks also run `check-yaml` and `check-github-actions`. **No gate yet**
  for YAML formatting or complete behavioral coverage of workflow paths.
- **Safety / HISS:** pin actions, minimize permissions, pass untrusted
  expressions through environment values rather than script interpolation,
  and bound jobs/steps. HISS-02/07/08/10/15 adapt to workflow orchestration;
  embedded scripts additionally follow their own language column.

### 8.13 TOML API definition

- **Role and standard:** [core/api/vmafx.toml](../core/api/vmafx.toml) is the
  API definition, not executable code; its `schema = 2` contract is checked
  by [validate.py](../scripts/codegen/vmafx_api/validate.py).
- **Gates:** `check-toml` in [.pre-commit-config.yaml](../.pre-commit-config.yaml)
  checks syntax. `python3 scripts/codegen/vmafx-api.py --check` validates
  generated outputs; `python3 scripts/codegen/vmafx-api.py --abi-check --against-ref origin/master`
  checks compatibility. [core/test/meson.build](../core/test/meson.build)
  registers generated-current, append-only and generator tests.
  [The generation guide](development/api-generation.md) owns the schema and
  update procedure. **No gate yet** for TOML formatting.
- **Safety / HISS:** keep types, sizes, ownership, error codes and versioning
  explicit; never hand-edit generated outputs. HISS-07/09/15 adapt to
  validating the described ABI, HISS-08 to declarative input and HISS-10 to
  failing validation. HISS-14's append-only contract remains mandatory.

### 8.14 Meson

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

### 8.17 Cross-language interface policy (ADR-0702, updated by ADR-1852)

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

Read each cell as a policy obligation, not a passing-check claim. **† means
no gate yet for the complete rule across that column's shipped scope**;
partial enforcement is described in section 8 and below. An unmarked cell
has a concrete rejection mechanism in the cited language gates, not proof
that every existing file complies. `n/a` cells require no missing gate;
the footnote explains why. All `adapted` and `n/a` cells have reason
footnotes. C++ includes host C++; GPU columns include their host wrappers.
`ObjC++/MSL` combines Objective-C++ and Metal shading language; `Py`, `sh`,
`Actions`, `API TOML`, `Container` and `TS/JS` abbreviate the headings above.

| Rule | C | C++ | CUDA | HIP | SYCL | ObjC++/MSL | Go | Rust | Py | sh | MATLAB | Actions | API TOML | Meson | Container | TS/JS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Power-of-10 1: Simple control flow | applies † | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^flow] † | adapted[^flow] † | adapted[^flow] † | adapted[^flow] † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | adapted[^flow] † |
| Power-of-10 2: Bounded loops | applies † | applies † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^orchestration] † | n/a[^declarative] | adapted[^orchestration] † | adapted[^orchestration] † | adapted[^bounds] † |
| Power-of-10 3: Hot-path allocation | applies † | adapted[^ownership] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^allocation] † | adapted[^ownership] † | adapted[^allocation] † | adapted[^allocation] † | adapted[^allocation] † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | adapted[^allocation] † |
| Power-of-10 4: Function length | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | applies † |
| Power-of-10 5: Assertion density | applies † | applies † | adapted[^device-assertions] † | adapted[^device-assertions] † | adapted[^device-assertions] † | adapted[^device-assertions] † | adapted[^invariants] † | adapted[^invariants] † | adapted[^invariants] † | adapted[^invariants] † | adapted[^invariants] † | adapted[^validation] † | adapted[^validation] † | adapted[^validation] † | adapted[^validation] † | adapted[^invariants] † |
| Power-of-10 6: Minimal scope | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^scope] † | adapted[^scope] † | adapted[^scope] † | adapted[^scope] † | adapted[^scope] † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | adapted[^scope] † |
| Power-of-10 7: Checked returns | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | n/a[^declarative] | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † |
| Power-of-10 8: Preprocessor restraint | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^metaprogramming] † | adapted[^metaprogramming] † | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] | n/a[^no-preprocessor] |
| Power-of-10 9: Pointer restraint | applies † | adapted[^ownership] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^pointers] † | adapted[^pointers] † | adapted[^pointers] † | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] |
| Power-of-10 10: Max strictness | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies | applies † | applies | applies † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | applies † |
| HISS-01: acyclic control flow | applies † | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^flow] † | adapted[^flow] † | adapted[^flow] † | adapted[^flow] † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | adapted[^flow] † |
| HISS-02: loop / I/O bounds | applies † | applies † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^bounds] † | adapted[^orchestration] † | n/a[^declarative] | adapted[^orchestration] † | adapted[^orchestration] † | adapted[^bounds] † |
| HISS-03: hot-path allocation | applies † | adapted[^ownership] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^allocation] † | adapted[^ownership] † | adapted[^allocation] † | adapted[^allocation] † | adapted[^allocation] † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | adapted[^allocation] † |
| HISS-04: 60 LOC / McCabe ≤10 / ≤50 statements | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | n/a[^declarative] | applies † |
| HISS-07: handled errors | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † | adapted[^abi] † | adapted[^errors] † | adapted[^errors] † | adapted[^errors] † |
| HISS-08: static execution | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^data] | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † |
| HISS-09: reference proofs | applies † | adapted[^ownership] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^device] † | adapted[^pointers] † | adapted[^pointers] † | adapted[^pointers] † | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] | adapted[^abi] † | n/a[^no-pointers] | n/a[^no-pointers] | n/a[^no-pointers] |
| HISS-10: diagnostic hygiene | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies | applies † | applies | applies † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | applies † |
| HISS-15: positive / negative / boundary tests | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^validation] † | adapted[^validation] † | adapted[^validation] † | adapted[^validation] † | applies † |
| Banned functions (§1.2 rule 30) | applies † | applies † | applies † | applies † | applies † | applies † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † | n/a[^data] | adapted[^banned] † | adapted[^banned] † | adapted[^banned] † |
| Warnings-as-errors (compiler / lint / format) | applies † | applies † | applies † | applies † | applies † | applies † | applies † | applies | applies † | applies | applies † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | adapted[^diagnostics] † | applies † |

[^device]: Bound launch dimensions, pixel indices, address spaces and device buffer use; preallocate before frame processing and prove stream/event/fence ordering. Device completion needs an explicit bound; host I/O cancellation alone cannot establish it.
[^ownership]: C++ RAII and Rust ownership replace manual allocation and dereference conventions. Keep scoring loops allocation-free, use bounded views, and prove FFI layouts and unsafe lifetimes.
[^device-assertions]: Use device-appropriate invariant checks and positive/negative/boundary host-device contract tests. A portable shader need not contain two C runtime assertions per function; host assertions cannot observe every device thread.
[^flow]: Use the language's structured control flow; absence of C `goto`/`longjmp` syntax does not establish an acyclic call graph. Recursion remains prohibited; closures, methods and callbacks also need review.
[^bounds]: Collection/frame bounds and explicit I/O deadlines replace a purely static loop-count proof for services and managed code. Retries, workers and queues must be capped; cancellation must reach the actual operation.
[^allocation]: Managed/script runtimes allocate implicitly. Bound working sets, reuse buffers and cap concurrency; the allocation-free rule still governs any native scoring loop they call. MATLAB numeric loops preallocate arrays.
[^invariants]: Use checked invariants and tests at ownership/shape/error boundaries rather than inserting two C `assert()` calls into every managed function. Public input validation must work when debug assertions are disabled.
[^scope]: Use the language's local/block scope and avoid mutable globals; Python, MATLAB and shell do not have C's declaration/block model.
[^errors]: Check error results, exceptions, rejected promises or process statuses according to the language. Workflow/build steps must propagate failure; an ignored pure value is distinct from an ignored error. Shell `set -e` is insufficient in conditional contexts.
[^no-preprocessor]: This surface has no C preprocessor. Generated source and embedded programs retain their own preprocessor rules; dynamic evaluation is covered by HISS-08 instead.
[^metaprogramming]: These languages have no C preprocessor. Keep templates, macros, generation, workflow expressions and build substitutions explicit and inspectable; prohibit evaluation of untrusted source text. Generated bindings remain checked against their definition.
[^pointers]: Go `unsafe`/cgo, Rust unsafe/FFI and Python ctypes need lifetime, bounds, aliasing and ownership proofs. Safe references do not acquire C's single-dereference syntax restriction.
[^no-pointers]: This language surface exposes no raw pointer arithmetic. Shell, MATLAB and JS callers validate data and lifetimes through their native interface; MEX C and any future native extension inherit the C rules.
[^declarative]: The configuration surface has no runtime scoring algorithm, function body, heap allocator or raw-pointer operation to which this particular rule can attach. Embedded scripts and generated source inherit their implementation-language obligations.
[^orchestration]: Bound workflow jobs/steps and subprocess/configuration work instead of proving a numeric kernel's iteration count. Existing timeout checks do not cover every nested script or configuration command.
[^validation]: Test accepted, rejected and boundary configurations and the behavior they generate; runtime assertion counts are not meaningful in a recipe or schema. Syntax validation alone is not behavioral testing.
[^diagnostics]: Fail on validator, builder, linter and formatter findings rather than compiler warnings on a data file. Embedded compilers retain their language's warning policy.
[^abi]: Validate the ABI's explicit status/error, pointer/handle, size and ownership declarations and test generated consumers. TOML itself neither handles a runtime error nor dereferences a pointer.
[^data]: The API definition is parsed as declarative TOML, not evaluated as source; syntax/schema validation is the applicable static-execution check. C library function bans are enforced on generated/hand-written consumers, not on TOML strings.
[^banned]: Use the equivalent safe operation: no dynamic `eval`/`exec`, unsafe shell interpolation or unchecked string-to-number conversion; use explicit argument vectors and checked results. Trusted build subprocesses are necessary but must not evaluate untrusted code.

### 9.1 Missing-gate cells and partial coverage

The following is the complete **† / no gate yet** list, grouped by identical
column sets. “Runtime columns” means C, C++, CUDA, HIP, SYCL, ObjC++/MSL,
Go, Rust, Py, sh, MATLAB and TS/JS. “Recipe columns” means Actions, Meson
and Container. A missing complete gate does not erase the existing checks
in section 8.

| Rules | Columns marked no gate yet |
|---|---|
| Power-of-10 1, 3, 4, 6; HISS-01, HISS-03, HISS-04 | All runtime columns |
| Power-of-10 2, 7; HISS-02, HISS-08; banned functions | All runtime and recipe columns |
| Power-of-10 5; HISS-15 | All columns |
| Power-of-10 8 | C, C++, CUDA, HIP, SYCL, ObjC++/MSL, Go, Rust |
| Power-of-10 9 | C, C++, CUDA, HIP, SYCL, ObjC++/MSL, Go, Rust, Py |
| HISS-07 | All columns |
| HISS-09 | C, C++, CUDA, HIP, SYCL, ObjC++/MSL, Go, Rust, Py, API TOML |
| Power-of-10 10; HISS-10; warnings-as-errors | All columns except Rust and sh |

In particular, do not promote these partial mechanisms to complete gates:

- [The HISS catalog](../.config/hiss/coverage.yaml) declares selected C,
  Go, Python and Rust matchers; it leaves method cycles, several discarded
  errors, dynamic imports, unsafe traits and native pointer proofs as gaps.
  It makes no complete per-language claim for GPU sources or recipes.
- [.clang-tidy](../.clang-tidy) checks 60-line functions but sets
  `StatementThreshold: 120` and `BranchThreshold: 15`; that is not the
  HISS-04 cap of 50 statements and McCabe complexity 10. Rust's
  [cognitive-complexity threshold](../clippy.toml) is a different metric.
- `make assertion-density` ([assertion-density.sh](../scripts/ci/assertion-density.sh))
  checks selected fork-authored C/C++ functions for at least one assertion
  when at least 20 lines long; the two-assertion average is informational.
  It does not enforce whole-tree Power-of-10 rule 5.
- [werror-args.sh](../scripts/ci/werror-args.sh) enables fatal compiler/linker
  diagnostics only in selected CI profiles; release/container profiles do
  not inherit it. It does not add fatal warnings to custom Metal shader
  commands. Go formatting mutates staged files, and mypy's Make target is
  advisory. Neither proves complete HISS-10 coverage.
- General formatting/lint/testing gaps are listed in each language section:
  Objective-C++ formatting, Metal shader lint, MATLAB tooling, YAML/TOML
  formatting, Meson-specific tooling, Containerfile tooling and tree-wide
  JS/TS tooling. Tests exist in several of these areas, but there is no
  universal gate that mandates all three HISS-15 cases for every interface.

Slice 2 of [#2438](https://github.com/VMAFx/vmafx/issues/2438) fills missing
checks and demonstrates rejection with planted defects. This slice adds no
gate, changes no scanner applicability and grants no exception. If a scanner
imposes a rule on a cell marked `n/a`, report the mismatch to its owning
project with the matrix cell and a reproducer; do not hide it by changing
this table or weakening the gate.

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
