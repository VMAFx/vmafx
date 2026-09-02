# ADR-0140: SIMD DX framework — header macros + scaffolding skill

- **Status**: Accepted
- **Date**: 2026-04-21
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, dx, build, agents

## Context

After [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) and
[ADR-0139](0139-ssim-simd-bitexact-double.md), the fork-local SIMD
surface has grown large enough that the per-feature boilerplate is
visible as a tax on future SIMD work. Three concrete costs surfaced:

1. **Repeated intrinsic patterns.** The bit-exactness invariants
   from ADR-0138 (single-rounded `float * float` → `_mm256_cvtps_pd`
   widen → `_mm256_add_pd`, no FMA) and ADR-0139 (per-lane scalar
   double reduction for double-promoted scalar C) are written
   verbatim in multiple TUs. Every new SIMD kernel that needs the
   same pattern re-derives it from scratch, risking silent drift.
2. **Repeated dispatch scaffolding.** Every new SIMD path requires
   a matching `_*_set_dispatch` setter, a function-pointer typedef
   in a `_simd.h` header, and CPU-flag plumbing in `cpu.c`. The
   shape is mechanical but error-prone — missing one step gives
   a scalar-only dispatch that silently eats the SIMD code at link
   time.
3. **Repeated test + meson scaffolding.** Each new SIMD TU needs a
   matching entry in `core/src/meson.build` under the correct
   `is_asm_enabled` / AVX-512 guard, plus a
   `core/test/meson.build` test target with the right
   `platform_specific_cpu_objects` inclusion and `host_machine.cpu_family`
   filter.

The inventory audit surfaced **nine** remaining SIMD gaps (convolve
NEON; ssim NEON bit-exactness audit; ssimulacra2 AVX2 + NEON;
motion_v2 NEON; vif_statistic AVX-512 + NEON; float_ansnr; moment;
luminance_tools; DX-level tooling). Cutting the boilerplate cost
before PR #B unlocks them at higher throughput than grinding each
one separately.

Two fork constraints shape the design:

- **User memory `feedback_simd_dx_scope.md`** — "SIMD framework" in
  this fork means fork-internal DX (macros / helpers / codegen), NOT
  Highway / simde / xsimd adoption. No cross-ISA portability layer.
- **[ADR-0125](0125-ms-ssim-decimate-simd.md) + ADR-0138 + ADR-0139**
  — bit-exactness with the scalar reference is non-negotiable. Any
  DX macro MUST make the bit-exactness invariant easier to preserve,
  not easier to lose.

## Decision

We will ship a two-part SIMD DX framework under **PR #A** (this
ADR):

1. **A new header `core/src/feature/simd_dx.h`** containing
   ISA-specific macros for the recurring patterns identified in
   ADR-0138 / ADR-0139. Macros are per-ISA by name (no cross-ISA
   abstraction): e.g.
   `SIMD_WIDEN_ADD_F32_F64_AVX2(acc_pd, a_ps, b_ps)`,
   `SIMD_PER_LANE_SCALAR_DOUBLE_REDUCE_AVX2(block)`,
   `SIMD_WIDEN_ADD_F32_F64_NEON`,
   `SIMD_PER_LANE_SCALAR_DOUBLE_REDUCE_NEON`.
   Compiler-inlined; zero runtime overhead. Each macro is documented
   with its scalar C equivalent and a link to the governing ADR.
2. **An upgraded `/add-simd-path` skill** (see
   [`.claude/skills/add-simd-path/SKILL.md`](../../.claude/skills/add-simd-path/SKILL.md))
   that scaffolds a new SIMD TU (`.c` + `.h`), a matching unit-test
   file, and the meson.build row edits from a short kernel-spec
   declaration. The scaffolding includes the right copyright
   header, the dispatch setter boilerplate, the CPU-flag guard, and
   the `platform_specific_cpu_objects` include in the test target.

Both are demonstrated in the same PR on two real kernels:

- **Demo 1 — convolve NEON.** Uses `SIMD_WIDEN_ADD_F32_F64_NEON`
  the per-lane-scalar-double pattern to match ADR-0138's
  bit-exactness invariant on aarch64. Runs under QEMU
  (`qemu-aarch64-static` + `aarch64-linux-gnu-gcc` cross toolchain).
- **Demo 2 — ssim NEON bit-exactness audit.** Research-0012
  follow-up. Cross-compile + run under QEMU; diff `--precision max`
  output against scalar; if the AVX2/AVX-512-style drift repeats,
  apply the same per-lane-scalar-double fix using the DX macros.

Neither demo introduces a new metric — both are existing kernels
retargeted or verified. The intent is to *prove the DX layer on
real code* before PR #B consumes it at scale.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Header-only macros (no skill upgrade) | Simpler scope; skill stays as-is | Per-feature bootstrap cost (meson / test / dispatch setter / header typedef) still dominates; 5 of 9 inventory gaps are full-new kernels where scaffolding is the bottleneck | Rejected — leaves the per-feature cost on the table |
| Skill-only codegen (no macros) | No new runtime .h surface; no pre-processor indirection | The bit-exactness patterns stay copy-pasted; ADR-0138 / ADR-0139 invariants are easy to lose on next port | Rejected — the riskiest cost (silent bit-exactness drift) stays uncut |
| **Both — header macros + skill upgrade (chosen)** | Cuts both the per-pattern and per-feature costs; demonstrable on two real kernels in the same PR; each future SIMD PR reuses both layers | ~40% larger PR #A scope; two artefacts to maintain | **Decision** — per user popup Q1.3 2026-04-21, the combined form is the only shape that unlocks PR #B at full throughput |
| Cross-ISA thin abstraction (Highway / simde / xsimd style) | Write-once-run-everywhere; tiny per-kernel LoC | Adds a portability-layer dependency + review surface; ISA-specific bit-exactness rules (FMA availability, rounding mode, lane ordering) hide inside the abstraction; user memory `feedback_simd_dx_scope.md` explicitly rules it out | Rejected — contradicts the fork's documented SIMD policy |
| ISA-specific macros without cross-ISA abstraction (chosen for macros) | Each macro's bit-exactness invariant is explicit and audit-able; names include the ISA (no `#if __AVX2__` dispatch inside one macro name) | More macro names to remember | **Decision** — per user popup Q1.3-Q2 2026-04-21 |

## Consequences

- **Positive**:
  - ADR-0138 / ADR-0139 bit-exactness patterns survive future
    SIMD ports via the macros' documented scalar equivalents.
  - PR #B's four gaps (convolve NEON, ssim NEON audit, ssimulacra2
    AVX2+NEON, motion_v2 NEON + vif_statistic AVX-512/NEON) reuse
    both the macros and the skill scaffolding; expected ~40%
    LoC reduction on the mechanical parts.
  - The `/add-simd-path` skill upgrade means new SIMD kernels
    start from a known-good scaffold instead of a cold copy-paste
    from the nearest feature; missing steps (dispatch setter,
    meson row, test target) are harder to forget.
- **Negative**:
  - One more header (`simd_dx.h`) to maintain; macro names become
    part of the fork's internal vocabulary.
  - The skill's `kernel-spec` DSL is a small new surface area that
    needs documentation + examples.
  - PR #A ships no user-visible speedup — its value is in PR #B
    and beyond.
- **Neutral / follow-ups**:
  - [`core/src/feature/AGENTS.md`](../../core/src/feature/AGENTS.md)
    rebase invariant: `simd_dx.h` is fork-local and must not
    conflict with upstream. On rebase, keep the fork's version.
  - [`docs/rebase-notes.md`](../rebase-notes.md) entry: document
    the DX framework and the skill upgrade so future rebase
    conflicts on `add-simd-path/SKILL.md` are resolved in favour
    of the fork's version.
  - [`CHANGELOG.md`](../../CHANGELOG.md) entry under Added.
  - Reproducer for PR #A:

    ```text
    # Cross-compile + run NEON audit under QEMU
    meson setup build-aarch64 \
      --cross-file=build-aux/aarch64-linux-gnu.ini \
      -Denable_cuda=false -Denable_sycl=false
    ninja -C build-aarch64
    qemu-aarch64-static -L /usr/aarch64-linux-gnu \
      build-aarch64/tools/vmaf --cpumask 255 \
      --reference REF --distorted DIS --feature float_ssim \
      --feature float_ms_ssim --precision max -o /tmp/scalar.xml
    qemu-aarch64-static ... --cpumask 0 ... -o /tmp/neon.xml
    diff <(grep -v '<fyi fps' /tmp/scalar.xml) \
         <(grep -v '<fyi fps' /tmp/neon.xml)     # must be empty
    ```

## References

- Source: user popup (2026-04-21, this session) — PR #B scope
  selection + DX form selection (`Q1.2-Q2` "Both — header + skill",
  `Q1.3-Q2` "ISA-specific macros").
- User memory: `feedback_simd_dx_scope.md` — rules out Highway /
  simde / xsimd; fork-internal DX only.
- Related ADRs:
  [ADR-0125](0125-ms-ssim-decimate-simd.md) — MS-SSIM decimate SIMD
  bit-exactness contract;
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — convolve
  widen-then-add pattern;
  [ADR-0139](0139-ssim-simd-bitexact-double.md) — per-lane
  scalar double reduction pattern;
  [ADR-0108](0108-deep-dive-deliverables-rule.md) — six
  deliverables apply to PR #A.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/src/feature/simd_dx.h` — present.
- `/add-simd-path` skill carries the kernel-spec flags documented in
  `.claude/skills/add-simd-path/SKILL.md`.
- Demo kernels (convolve NEON, ssim NEON bit-exactness audit) are
  tracked under follow-up ADRs (ADR-0145 motion_v2 NEON, ADR-0159
  / 0160 psnr_hvs AVX2 / NEON, ADR-0161 / 0162 / 0163 ssimulacra2
  SIMD), all of which cite the framework.
- Verification command: `ls core/src/feature/simd_dx.h`.
