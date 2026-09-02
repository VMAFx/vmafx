<!-- markdownlint-disable MD060 -->
# ADR-0120: DNN-enabled matrix legs across compilers + macOS

- **Status**: Accepted
- **Date**: 2026-04-19
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, ai, dnn, ort, build, github-actions

## Context

The fork's tiny-AI surface (`core/src/dnn/`, `ai/`) compiles only when
`-Denable_dnn=enabled` is passed to meson. Until this ADR, the only CI job
that actually built with that flag was the dedicated `Tiny AI (DNN Suite +
ai/ Pytests)` job in [tests-and-quality-gates.yml](../../.github/workflows/tests-and-quality-gates.yml).
That job runs on a single combination: ubuntu-latest, gcc, ONNX Runtime
1.22.0 (Microsoft tarball).

The single-leg coverage hides three real failure modes:

1. **Compiler portability** — clang and gcc disagree on a handful of
   warning categories the ORT C-API headers exercise (e.g.
   `-Wzero-as-null-pointer-constant`, `-Wstrict-prototypes`). A clean gcc
   build does not prove a clean clang build.
2. **macOS portability** — Homebrew ships ORT as a keg-only formula
   built against Apple's libc++ with different default visibility flags
   than the Linux MS tarball. ORT-on-macOS bit-rotted twice in the past
   year because no leg exercised it; both regressions surfaced from
   downstream user reports rather than CI.
3. **DNN suite coverage breadth** — the `dnn` meson test suite (9 tests
   under `core/test/dnn/meson.build`) only ran on the Tiny AI job's
   gcc-only build, so a DNN test failure that depends on libstdc++ vs
   libc++ STL behaviour would not surface until release.

The user explicitly scoped this PR as **"Add 3 legs: gcc + clang +
macOS, all DNN-on"** (see References). The macOS leg keeps
`experimental: true` so a Homebrew ORT formula bump that breaks ABI
doesn't block merge while we build out the macOS-side ORT pinning
story (separate, future ADR).

## Decision

Add three new entries to the `libvmaf-build` matrix in
[libvmaf-build-matrix.yml](../../.github/workflows/libvmaf-build-matrix.yml):

| Display name | OS | Compiler | ORT install | Required |
|---|---|---|---|---|
| `Build — Ubuntu gcc (CPU) + DNN` | ubuntu-latest | gcc | MS tarball 1.22.0 | yes |
| `Build — Ubuntu clang (CPU) + DNN` | ubuntu-latest | clang | MS tarball 1.22.0 | yes |
| `Build — macOS clang (CPU) + DNN` | macos-latest | clang | Homebrew (`brew install onnxruntime`) | no (`experimental: true`) |

All three entries set `dnn: true` and `meson_extra: -Denable_dnn=enabled`.
Conditional steps `Install ONNX Runtime (linux, DNN leg)` and
`Install ONNX Runtime (macos, DNN leg)` install ORT only on the matching
combination, mirroring the install pattern from the existing Tiny AI
job. A new `Run meson dnn suite (DNN leg)` step runs `meson test
--suite=dnn --print-errorlogs` for log-isolation; the existing `Run
tests` step (which calls `ninja test`) also exercises the suite, so the
dedicated step exists only to make a dnn-suite regression easy to spot
in the matrix UI.

The two Linux DNN legs are pinned to **required** status checks on the
`master` branch protection (re-pinned atomically alongside this ADR's
landing). The macOS leg stays informational because Homebrew ORT
floats — a sudden 1.23 ABI bump should not gate merges. If macOS ORT
stabilises (or we switch to a pinned macOS source build), a follow-up
ADR can promote it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Status quo (Tiny AI single leg only)** | Zero CI cost, simple. | Compiler-portability and macOS regressions only surface post-release. | The whole motivation for this ADR is closing exactly that hole. |
| **Add only the gcc DNN leg as a sanity duplicate** | Minimal cost. | Already covered by the Tiny AI job — pure duplication, no new signal. | Net negative: more minutes, no new failure modes detected. |
| **Add gcc + clang Linux only, skip macOS** | Half the new minutes; avoids Homebrew flakiness. | macOS ORT bit-rot keeps recurring; user explicitly asked for macOS coverage. | User scope = all three. macOS gated as `experimental` mitigates the flakiness concern. |
| **Add windows-latest DNN leg too** | Maximum platform coverage. | Windows ORT install pattern needs a separate, larger investigation (vcpkg vs prebuilt vs MS NuGet); doubles the PR scope. | Out of scope for this ADR — Windows DNN coverage is its own future ADR. |
| **Run only the `dnn` suite on the new legs (skip `ninja test`)** | Faster per-leg. | We'd lose the cross-OS check for non-DNN feature extractors built with `-Denable_dnn=enabled`, which is the whole point of compiler-portability coverage. | Build the world, test the world. |

## Consequences

**Positive:**

- Compiler-portability and macOS-portability regressions in the DNN
  surface surface in CI, not in user reports.
- The `dnn` meson suite now runs on three OS/compiler combinations
  instead of one.
- The Tiny AI job in `tests-and-quality-gates.yml` is no longer the
  sole gate for DNN buildability — its scope can shrink to "verify ORT
  EP fallback semantics" (its actual unique value) without losing
  build-portability coverage.

**Negative:**

- Three additional matrix runs per push to master / PR. Estimated cost:
  ~12 minutes wall-clock added per CI run (parallel matrix), ~30 GHA
  minutes added per run. Acceptable on the public fork's free tier.
- Homebrew ORT floats; a Homebrew ORT 1.23+ release that breaks ABI
  will turn the macOS leg yellow until we either pin ORT via source
  build or update the meson `dnn` integration. `experimental: true`
  prevents this from blocking merges.

**Neutral / follow-ups:**

- Branch protection re-pinned atomically with this ADR's merge to add
  `Build — Ubuntu gcc (CPU) + DNN` and `Build — Ubuntu clang (CPU) +
  DNN` as required contexts (19 → 21 required checks). The macOS leg
  is intentionally not added to required.
- A follow-up ADR will address Windows DNN coverage once we settle the
  ORT install pattern there.
- A follow-up ADR may pin macOS ORT to a known-good Homebrew version
  (or switch to source build) and promote the macOS leg to required.

## References

- [ADR-0112](0112-ort-backend-testability-surface.md) — ORT backend
  testability surface that the dnn suite exercises.
- [ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md) — ORT EP
  fallback semantics; the Tiny AI job's unique value going forward.
- [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md) —
  matrix-consolidation ADR; this PR adds new entries to the consolidated
  matrix established there.
- [ADR-0116](0116-ci-workflow-naming-convention.md) — Title Case
  display names; the three new legs follow that convention.
- [tests-and-quality-gates.yml#L192-L194](../../.github/workflows/tests-and-quality-gates.yml#L192-L194)
  — install + test pattern that the new matrix legs mirror.
- `req` (paraphrased): user picked "Add 3 legs: gcc + clang + macOS,
  all DNN-on" via the post-cascade scope popup, after I offered the
  three-leg option as the most-ambitious of three scope choices.
- Per-surface doc impact: this ADR documents the workflow-file change
  and the branch-protection delta. The DNN surface itself is unchanged
  (`docs/ai/` already documents the ORT integration); no `docs/ai/`
  edit needed.
