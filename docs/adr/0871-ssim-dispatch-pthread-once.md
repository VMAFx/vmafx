<!-- markdownlint-disable MD060 -->
# ADR-0871: SSIM SIMD dispatch installation must be pthread_once-guarded

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: simd, threading, correctness, ssim, tsan

## Context

A TSan (ThreadSanitizer) audit of the libvmaf thread pool on
2026-05-30 (worktree `fix/tsan-race-audit`, master tip
`bbcaa8d127`) ran the CLI under

```text
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7" \
./build-tsan/tools/vmaf -r checkerboard_1920_1080_10_3_0_0.yuv \
                       -d checkerboard_1920_1080_10_3_1_0.yuv \
                       -w 1920 -h 1080 -p 420 -b 8 --threads 16 \
                       --feature float_ssim --feature float_ms_ssim \
                       --feature psnr --feature ciede
```

and surfaced **ten** TSan data-race warnings on four process-wide
function-pointer globals defined in
[`core/src/feature/iqa/ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c):

| Global | Type | Set by |
| --- | --- | --- |
| `g_ssim_precompute` | `ssim_precompute_fn` | `iqa_ssim_set_dispatch` |
| `g_ssim_variance` | `ssim_variance_fn` | `iqa_ssim_set_dispatch` |
| `g_ssim_accumulate` | `ssim_accumulate_fn` | `iqa_ssim_set_dispatch` |
| `g_iqa_convolve` | `iqa_convolve_fn` | `iqa_convolve_set_dispatch` |

The races fire because the per-extractor `init()` callback for both
`float_ssim` and `float_ms_ssim` calls the dispatch setters at
worker-thread setup time. With `--threads >= 4` and both features
enabled, multiple workers enter `vmaf_feature_extractor_context_init`
concurrently (the `is_initialized` check itself is unguarded), so
every worker race-writes the same dispatch pointer.

The races are **value-benign** in current builds — every worker
installs the same ISA-best function pointer. But:

1. On a 64-bit pointer with weakly-ordered hardware (ARM, POWER) a
   non-aligned racing store could publish a torn pointer; a worker
   then calling through that pointer would jump to a synthesised
   address. The SSIM dispatch globals are 8-byte aligned in
   practice but the racy publish is undefined behaviour by the C
   memory model regardless.
2. ADR-0138 elevates SIMD-dispatch correctness to a load-bearing
   invariant. The audit treats any data race on a dispatch global
   as a real bug, never a benign warning.
3. The pattern of "every worker installs the same ISA-detect
   result" is itself a smell — the install should fire exactly
   once and the workers should observe the result through a
   memory barrier.

## Decision

Gate the four SSIM dispatch globals behind a single
**process-wide `pthread_once_t`** owned by `ssim_tools.c`. Both
calling translation units (`float_ssim.c` and `float_ms_ssim.c`)
call a new helper

```c
void iqa_ssim_install_dispatch_once(pthread_once_t *guard,
                                    void (*installer)(void));
```

declared in [`core/src/feature/iqa/ssim_simd.h`](../../core/src/feature/iqa/ssim_simd.h).
Internally the helper:

1. Publishes the (idempotent) installer callback via an atomic CAS
   on a `_Atomic(void (*)(void))`, so only the first publisher
   writes the slot and TSan sees a single sequenced store.
2. Calls `pthread_once` against the file-static
   `g_ssim_dispatch_once` in `ssim_tools.c` — **not** the per-TU
   guard that the caller passes in. The per-TU guard parameter is
   retained for API symmetry but ignored; the shared guard is what
   guarantees cross-TU mutual exclusion.
3. `pthread_once` runs the trampoline exactly once and emits a
   full memory barrier; all subsequent workers (in any TU) read
   the four dispatch globals through that barrier and see fully
   constructed pointers.

The installer callbacks across the two TUs are byte-for-byte
identical (same ISA detection, same dispatch tables) so whichever
TU's worker reaches `pthread_once` first wins the install and the
other becomes a no-op.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `pthread_once_t` shared via `ssim_tools.c` (chosen) | Single fire across both TUs; full memory barrier; no compile-time coupling to ISA headers in `iqa/`; preserves Tom-Distler import hygiene (no SIMD `#include` added to `iqa/*.c`) | Slightly more indirection than per-TU guard | Selected — only design that races zero on the dispatch globals AND respects the iqa/ import-hygiene constraint from ADR-0125 |
| Per-TU `pthread_once_t` | Simplest | Each TU's guard fires independently; cross-TU race on dispatch globals persists (confirmed empirically — 4 TSan warnings remained) | Rejected: race not actually fixed |
| Atomic stores on the dispatch globals | No barriers, no setup ordering | Doesn't fix the "called from every worker" antipattern; pointer-tearing UB on weakly-ordered hardware would be fixed but the redundant work and "first-store-wins" model remain | Rejected: doesn't fix root cause |
| Install dispatch at `vmaf_init()` library time | Single fire by construction | Requires touching `libvmaf.c` init path and creating a cross-feature init hook that has no other reason to exist; breaks the "feature init owns its own setup" invariant | Rejected: larger blast radius, weaker locality |
| Move dispatch installation into `iqa/ssim_tools.c` itself with ISA detection inside `iqa/` | No header changes | Pulls `core/src/feature/x86/*` and `core/src/feature/arm64/*` headers into the Tom-Distler-imported `iqa/` directory, breaking ADR-0125 §Ground-rules import hygiene | Rejected: violates ADR-0125 |

## Consequences

- **Positive**:
  - TSan stress test (`--threads 16`, both float_ssim + float_ms_ssim,
    1080p checkerboard) drops from 10 warnings to 0.
  - Bit-for-bit identical output (verified against
    src01_hrc00_576x324 / src01_hrc01_576x324 input pair:
    `pooled vmaf.mean = 76.66783` matches pre-fix).
  - All 63 `meson test -C build-tsan` cases pass clean under TSan.
  - Future SIMD additions (e.g. SVE) only need to extend the
    installer body — the once-fire guarantee comes for free.
- **Negative**:
  - Adds a new `#include <pthread.h>` to two feature TUs and the
    iqa header `ssim_simd.h`. libvmaf is already linked against
    pthread on all hosted platforms, so no new link-time
    requirement.
  - `iqa/ssim_tools.c` now depends on `<stdatomic.h>` for the
    publish CAS — C11 minimum, already required by other libvmaf
    TUs.
- **Neutral / follow-ups**:
  - No other process-wide SIMD-dispatch globals discovered in the
    audit; the pattern is unique to SSIM among the existing CPU
    feature extractors (`integer_*` extractors use compile-time
    dispatch; CAMBI dispatch is per-state). If a future extractor
    introduces a similar runtime-installed global, this ADR's
    pthread_once helper is the canonical pattern to follow.

## References

- TSan audit transcript and per-race triage in
  [`docs/research/tsan-race-audit-2026-05-30.md`](../research/tsan-race-audit-2026-05-30.md).
- Underlying SIMD invariants:
  [ADR-0125](0125-ms-ssim-decimate-simd.md) (iqa/ import hygiene),
  [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md)
  (`g_iqa_convolve` install path),
  [ADR-0139](0139-vif-avx2-numerical.md) (bit-exactness as a
  load-bearing invariant).
- Related prior race fix:
  [ADR-0607](0607-shared-resource-outlive-worker-scope.md)
  (`feedback_shared_resource_outlive_worker_scope`), PR #1415.
- Source: agent task `chore/mcp-tools-audit-20260529` —
  "Run TSan on the libvmaf threadpool + dispatcher paths to
  surface latent race conditions" (operator directive 2026-05-30).
