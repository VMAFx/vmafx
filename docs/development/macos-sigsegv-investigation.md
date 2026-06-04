<!-- markdownlint-disable MD013 -->
# macOS CI Build Failure Investigation

**Tracker:** T-MACOS-SIGSEGV-UNRESOLVED-2026-05-19
**Status:** RESOLVED — 2026-06-04
**Resolved by:** PR #654 / commit `695d29626`
(`fix(build): restore integer_ssim_moments_t type definition (macOS Clang unblock)`)

---

## Background

Since 2026-05-19, three macOS CI jobs have been failing:

- `Build — macOS clang (CPU)`
- `Build — macOS clang (CPU) + DNN`
- `Build — macOS Metal (runtime)`

The original tracking entry (T-MACOS-SIGSEGV-UNRESOLVED-2026-05-19) described
persistent SIGSEGVs after three earlier fix PRs (#1355, #1403, #1412). Those
historical crashes were in `core/src/output.c` (off-by-one heap overread
surfaced by `MALLOC_PERTURB_=198` on Apple Clang, fixed in ADR-0602 /
ADR-0606). The tmate SSH debug step was added in ADR-0626 to obtain a live
`lldb` backtrace on the next `workflow_dispatch` run.

---

## Investigation — 2026-06-04

### Step 1: Identify the failing workflow

Primary file: `.github/workflows/libvmaf-build-matrix.yml`
Three macOS jobs use `os: macos-latest` (which resolves to `macos-15-arm64` on
runner image `20260527.0100.1`, released 2026-05-27).

### Step 2: Fetch log from the most recent fully-run macOS failure

CI run **26930504863** (workflow `libvmaf-build-matrix.yml`, triggered by
PR #642 `fix/vmaf-init-double-init-guard-vmaf-close-pointer-contract`) had all
three macOS jobs fail with `conclusion: failure`.
Job ID for `Build — macOS clang (CPU)`: `79449068642`.

Key error from the log (`gh run view --job 79449068642 --log-failed`):

```text
FAILED: [code=1] src/liblibvmaf_feature.a.p/feature_integer_ssim.c.o
../src/feature/integer_ssim.c:167:37: error: unknown type name 'integer_ssim_moments_t'
  167 |                                     integer_ssim_moments_t *buf);
      |                                     ^
../src/feature/integer_ssim.c:171:38: error: unknown type name 'integer_ssim_moments_t'
...
8 errors generated.
```

The Metal job (`79449068692`) produced the **identical** 8 errors.
**This is a compile error, not a SIGSEGV.**

### Step 3: Root cause

`integer_ssim_moments_t` was defined exclusively in
`core/src/feature/x86/integer_ssim_avx2.h`, included under:

```c
#if ARCH_X86
#include "x86/integer_ssim_avx2.h"
#endif
```

`integer_ssim.c` uses the type unconditionally for function-pointer typedefs
(lines 166–172) and scalar wrapper functions (lines 181–203). On macOS arm64
and Windows arm64, `ARCH_X86` is 0, so the header — and the type — was
invisible to the compiler.

**Runner context:** `macos-latest` resolved to `macos-15-arm64` (Apple Silicon
M-series) on image `20260527.0100.1` (2026-05-27). All three macOS matrix legs
ran on ARM64; none had `ARCH_X86=1`.

### Step 4: Fix

Promote `integer_ssim_moments_t` to a new shared header
`core/src/feature/integer_ssim.h` included unconditionally. The x86 header
(`integer_ssim_avx2.h`) is updated to pull the typedef from the shared header
via `#include "../integer_ssim.h"`. This preserves the x86 SIMD path's access
to the type without duplicating the definition.

**Landed:** PR #654 / commit `695d29626`, 2026-06-04.

---

## Instrumentation added (still useful for future failures)

The tmate SSH debug step added by ADR-0626 remains in place in
`.github/workflows/libvmaf-build-matrix.yml`:

```yaml
- name: SSH debug session on test failure
  if: ${{ failure() && runner.os == 'macOS' && github.event_name == 'workflow_dispatch' }}
  uses: mxschmitt/action-tmate@c0afd6f790e3a5564914980036ebf83216678101  # v3
  with:
    limit-access-to-actor: true
    connect-timeout-seconds: 1800
```

To use it for any future macOS failure:

```bash
gh -R VMAFx/vmafx workflow run "Builds" --ref <branch>
# Wait for "SSH debug session on test failure" step to print the tmate URL
# Then: ssh <id>@nyc1.tmate.io
# Then: lldb core/build/test/<failing_test>
#        run
#        bt
```

---

## Lessons learned

1. The runner image that triggered the failure (`macos-15-arm64/20260527.0100.1`)
   resolves `macos-latest` to Apple Silicon ARM64. Shared types must not be
   gated behind `#if ARCH_X86`.

2. Build failures on macOS were misclassified as SIGSEGV from the older tracking
   entry. The actual failures were compile errors; the earlier SIGSEGV row covered
   a separate, already-closed `output.c` bug cluster (ADR-0602 / ADR-0606).

3. The `libvmaf-build-matrix.yml` workflow uses `continue-on-error: true` for
   macOS legs (`matrix.experimental == true`), so macOS build failures are
   advisory and do not block PR merge. This allowed the regression to persist
   undetected across many PRs.

---

## References

- T-MACOS-SIGSEGV-UNRESOLVED-2026-05-19 — `docs/state.md` Open row (now closed)
- T-INTEGER-SSIM-MOMENTS-TYPE-NON-X86-2026-06-04 — `docs/state.md` Recently closed row
- ADR-0626: tmate SSH debug step (macOS CI)
- ADR-0602: vmaf\_write\_output off-by-one SIGSEGV (historical)
- ADR-0606: deep-fix for output.c SIGSEGV cluster (historical)
- ADR-1040: integer\_ssim\_moments\_t shared header promotion
- PR #654: `fix(build): restore integer_ssim_moments_t type definition`
- CI run 26930504863: macOS failure log source
