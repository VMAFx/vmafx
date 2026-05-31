<!-- markdownlint-disable MD013 MD060 -->
# ADR-0710: VMAFX CI Slim-Down v2 — One Build per OS + State-of-the-Art Sanitizers

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `sanitizers`, `vmafx`

## Context

ADR-0689 (VMAFX CI Matrix Deduplication) was a first dedup pass that removed
proper-subset build legs from `libvmaf-build-matrix.yml`, reducing from 20 to 15
active build rows. A subsequent decision (container-first posture per ADR-0686
and ADR-0701) made the production artefact the `vmafx-dev-mcp` Docker image
rather than per-backend host binaries. In that context, running 15 separate
build rows per PR is disproportionate to the signal each provides.

Additionally, the sanitizer jobs in `tests-and-quality-gates.yml` used a 3-way
matrix `[address, undefined, thread]` that fired all three on every PR, including
TSan whose overhead makes it unsuitable for per-PR blocking.

The user direction was to slim CI down to one full build per OS. The ADR-0686 /
ADR-0701 container-first posture supports this: the container build already
exercises all backends and toolchain combinations; the per-PR matrix should prove
the source compiles on each target OS, not replicate every permutation.

## Decision

We consolidate the CI matrix to the following structure.

### Build matrix (`build.yml`, replaces `libvmaf-build-matrix.yml`)

Three rows:

| Row | OS | Compiler | Backends | Tests |
|---|---|---|---|---|
| `Build — Linux (GCC, all backends)` | ubuntu-latest | GCC | CUDA + SYCL + Vulkan + HIP + CPU + DNN | Full meson suite + tox + Netflix golden |
| `Build — macOS (Clang, CPU + Metal)` | macos-latest | Apple Clang | CPU + Metal scaffold | meson suite + tox |
| `Build — Windows (MSVC + CUDA)` | windows-2025 | MSVC | CPU + CUDA (build-only) | CPU unit tests |

The Linux full-build leg proves the entire VMAFX source tree compiles with all
backends enabled on the primary development platform. Netflix golden assertions
run as part of its meson test suite (CLAUDE.md §8 / ADR-0024).

### Sanitizers (`sanitizers.yml`, replaces embedded jobs in `tests-and-quality-gates.yml`)

| Job | Trigger | Sanitizers |
|---|---|---|
| `Sanitizers — ASan + UBSan (PR gate)` | Every non-draft PR | `-fsanitize=address,undefined` combined |
| `Sanitizers — TSan (master push)` | Push to master only | `-fsanitize=thread` |
| `Fuzz — * (nightly)` | `cron: "30 4 * * *"` | libFuzzer + ASan (3 harnesses) |

MSan and CFI are not added now: MSan has a ~90% false-positive rate without an
instrumented stdlib; CFI is deferred to after the C23 bump (ADR-0692) and the
Rust FFI boundary (ADR-0705) stabilise.

### Dropped jobs

| Dropped job | Rationale |
|---|---|
| `Build — Ubuntu ARM clang (CPU)` | Not a VMAFX production target; ARM regressions caught in container builds |
| `Build — Ubuntu i686 gcc (CPU, no-asm)` | i686 is not a VMAFX production target; ADR-0151 contract deprioritised |
| `Build — Ubuntu gcc (CPU) + DNN` | Superseded by Linux full-build DNN leg with gcc |
| `Build — Ubuntu clang (CPU) + DNN` | Clang DNN leg redundant against gcc full-build |
| `Build — macOS clang (CPU) + DNN` | Superseded by `Build — macOS (Clang, CPU + Metal)` |
| `Build — Ubuntu Vulkan (T5-1b runtime)` | Folded into Linux full-build (`-Denable_vulkan=enabled`) |
| `Build — macOS Vulkan via MoltenVK (advisory)` | Too fragile; no required gate; dropped entirely |
| `Build — Ubuntu HIP (T7-10b runtime)` | Folded into Linux full-build (`-Denable_hip=true`) |
| `Build — macOS Metal (T8-1 scaffold)` | Folded into `Build — macOS (Clang, CPU + Metal)` |
| `Build — Ubuntu gcc Static (CPU)` | Static pkgconfig validated within Linux full-build |
| `Build — Ubuntu CUDA Static` | NVCC-static interaction covered by Linux full-build CUDA path |
| `Build — Ubuntu SYCL` | Folded into Linux full-build SYCL path |
| `Build — Ubuntu SYCL + CUDA` | Folded into Linux full-build combined flags |
| `Build — Windows MinGW64 (CPU)` | Superseded by `Build — Windows (MSVC + CUDA)`; MinGW64 is not a VMAFX production target |
| `Build — Windows MSVC + oneAPI SYCL (build only)` | SYCL folded into Linux full-build; MSVC+oneAPI edge case deprioritised |
| `Sanitizers — ASan + UBSan + MSan (thread)` (per-PR) | Moved to master-push-only TSan job |
| `Sanitizers — ASan + UBSan + MSan (address/undefined)` (separate rows) | Merged into single combined asan-ubsan PR job |
| `Cppcheck (Whole Project)` | Clang-tidy superset; no Cppcheck-only finding recorded in fork history |

### Required Checks Aggregator

`required-aggregator.yml` is updated: new check names match the 3 build jobs and
the combined asan-ubsan PR gate. `Cppcheck (Whole Project)` is removed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep 15-row matrix, add nightly-only flag per row | No PR risk | Does not reduce per-PR cost; signals already in container build | Goal is per-PR latency reduction |
| Keep separate per-backend legs | Faster failure triage per backend | 5–7× more runner time; each leg is a proper subset of the Linux full-build | Full-build provides all signals in one run |
| Keep ARM CI leg | Catches NEON regressions | No NEON-specific test fixtures in CI | Can be re-added when ARM fixtures land |
| Merge TSan into asan-ubsan | Single sanitizer job | TSan + ASan are incompatible at link level; combined build requires two invocations | Kept as separate master-push job |

## Consequences

- **Positive**: PR matrix reduced from 15 rows (post-ADR-0689) to 3. Sanitizer
  matrix reduced from 3 per-PR rows to 1 combined gate. Cppcheck removed. Expected
  per-PR runner-time reduction: ~70% versus the pre-ADR-0689 baseline (88 total
  jobs → ~26 for PR-triggering workflows).
- **Negative**: ARM and i686 cross-compile regressions no longer caught on PRs.
  Per-backend build isolations merged into the Linux full-build.
- **Neutral / follow-ups**: Windows MinGW64 required-check entry removed from branch
  protection. TSan master-push and fuzz nightly feed into nightly CI alongside the
  existing nightly jobs.

## References

- Parent umbrella: ADR-0686 (VMAFX rebrand + aggressive modernization)
- Container-first posture: ADR-0701 (cloud-native redesign)
- Prior dedup pass: ADR-0689 (VMAFX CI Matrix Deduplication)
- ADR-0347 (UBSan function-check suppression)
- ADR-0313 (Required Checks Aggregator)
- ADR-0270, ADR-0311 (libFuzzer nightly)
- `req` (paraphrased): user direction to slim CI to 1 full build per OS with a state-of-the-art sanitizer profile
