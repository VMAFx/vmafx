# ADR-0156: CUDA backend: graceful error propagation (Netflix#1420)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cuda, correctness, api, netflix-upstream, reliability

## Context

Netflix upstream issue
[#1420](https://github.com/Netflix/vmaf/issues/1420) reports that
running two VMAF-CUDA analyses concurrently aborts the second
process with:

```text
ffmpeg: ../src/cuda/common.c:166: vmaf_cuda_buffer_alloc:
Assertion `0' failed.
```

Root cause: the `CHECK_CUDA` macro in
[`core/src/cuda/cuda_helper.cuh`](../../core/src/cuda/cuda_helper.cuh)
called `assert(0)` on *any* CUDA error:

```c
#define CHECK_CUDA(funcs, CALL)                         \
    do {                                                \
        const CUresult cu_err = funcs->CALL;            \
        if (CUDA_SUCCESS != cu_err) {                   \
            const char *err_txt;                        \
            funcs->cuGetErrorName(cu_err, &err_txt);    \
            printf("code: %d; description: %s\n",       \
                   (int)cu_err, err_txt);               \
            assert(0);                                  \
        }                                               \
    } while (0)
```

Legitimate failure modes — `cuMemAlloc` OOM, `cuStreamCreate`
resource exhaustion, `cuModuleLoadData` compile errors on a
mismatched driver — all collapsed into `assert(0)`. Two
consequences:

1. **No graceful recovery**: a downstream caller that wanted to
   retry, fall back to CPU, or surface a clean error to the user
   had no way to do so; the process was already dead.
2. **`NDEBUG` footgun**: under a release build with `NDEBUG`
   defined, `assert(0)` is a no-op — the function then silently
   continued with an un-allocated buffer, leading to a segfault
   on the first dereference.

`vmaf_cuda_buffer_alloc` already returned `int` and every call
site used the `ret |= ...` pattern (see
`core/src/feature/cuda/integer_motion_cuda.c:159–167`,
`integer_vif_cuda.c:150–155`, `integer_adm_cuda.c:1035–1054`).
Callers were *ready* to handle a failure — but the macro never
let the failure propagate.

Fork PRs #60 and #62 ([ADR-0122](0122-cuda-framesync-segfault-hardening.md),
[ADR-0123](0123-cuda-null-guard.md)) hardened the null-state
path in `common.c` but left the assert-on-any-error semantics
untouched. Netflix#1420 is the remaining half of that hardening
story.

## Decision

Replace `CHECK_CUDA`'s abort-on-error with **graceful error
propagation** across the entire CUDA backend. Two new macros in
[`core/src/cuda/cuda_helper.cuh`](../../core/src/cuda/cuda_helper.cuh):

- **`CHECK_CUDA_GOTO(funcs, CALL, label)`** — on CUDA failure,
  logs the error (file / line / CUresult name / call text) and
  jumps to `label:` for cleanup. The caller declares `int
  _cuda_err = 0;` at function entry and puts cleanup code
  (context pop, buffer free, etc.) under `label:` before
  returning `_cuda_err`. Used for call sites with pending
  cleanup state.
- **`CHECK_CUDA_RETURN(funcs, CALL)`** — on CUDA failure, logs
  and returns directly from the enclosing function. Used for
  sites with no pending state to clean up (kernel launch
  dispatches, post-pop stream syncs).

A third helper maps `CUresult` → `-errno`:

```c
static inline int vmaf_cuda_result_to_errno(int cu_err_code)
{
    switch (cu_err_code) {
        case 0:   return 0;          /* CUDA_SUCCESS */
        case 2:   return -ENOMEM;    /* CUDA_ERROR_OUT_OF_MEMORY */
        case 3:
        case 4:   return -ENODEV;    /* NOT_INITIALIZED / DEINITIALIZED */
        case 1:
        case 101:
        case 201:
        case 400: return -EINVAL;    /* INVALID_* */
        default:  return -EIO;
    }
}
```

Every one of the 175 existing `CHECK_CUDA(...)` call sites
across **7 files** (`common.c`, `picture_cuda.c`, `libvmaf.c`,
`integer_motion_cuda.c`, `integer_vif_cuda.c`,
`integer_adm_cuda.c`, `cuda_helper.cuh` macro def) is converted:

- **122 sites → `CHECK_CUDA_GOTO`** with cleanup labels.
- **56 sites → `CHECK_CUDA_RETURN`**.

Twelve `static` helper functions that previously returned `void`
and used `CHECK_CUDA` internally are promoted to `int` so
failures reach their callers:

- `integer_motion_cuda.c`: `calculate_motion_score` (+
  matching function-pointer type in `MotionStateCuda`).
- `integer_vif_cuda.c`: `filter1d_8`, `filter1d_16`.
- `integer_adm_cuda.c`: `dwt2_8_device`,
  `adm_dwt2_s123_combined_device`, `adm_dwt2_16_device`,
  `adm_csf_device`, `i4_adm_csf_device`,
  `adm_csf_den_s123_device`, `adm_csf_den_scale_device`,
  `i4_adm_cm_device`, `adm_cm_device`,
  `integer_compute_adm_cuda`.

Public ABI is unchanged: every function exported via
`core/include/libvmaf/libvmaf_cuda.h` already returned `int`
and continues to return `int` with the same sign convention.
Previously-undocumented failure modes (OOM, stream creation
failure, kernel dispatch failure) now reach callers as
`-ENOMEM` / `-EIO` / `-EINVAL` / `-ENODEV` instead of aborting
the process.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Wholesale CHECK_CUDA replacement (this ADR)** | Fixes the reported bug and every analogous site in one pass; no mixed abort/graceful semantics left to reason about; the NDEBUG footgun is eliminated everywhere | 178 call sites + 12 void→int signature changes → large refactor; proportional review cost | **Chosen** — the user explicitly selected wholesale scope via popup; incremental coverage would leave the NDEBUG footgun alive in every untouched site |
| **Surgical: only `vmaf_cuda_buffer_alloc`** | Smallest diff; addresses the exact reported crash | Every other `cuMemAlloc` / `cuStreamCreate` / kernel-launch site keeps the abort-on-error semantics; reporter's symptom recurs on any other CUDA failure | Rejected via popup — user chose wholesale |
| **Keep `assert(0)` but add `fprintf` + `abort()`** | Preserves "fail loud" behaviour; no caller changes | Still aborts the process; NDEBUG no longer gates assert, but downstream still can't retry / fall back / degrade gracefully | Rejected — doesn't solve the reported problem, just changes the flavour of abort |
| **Introduce a callback for error policy (abort vs return)** | Configurable per-caller | Adds a runtime branch on every CHECK; new public API surface; over-engineered for a yes/no problem | Rejected — the graceful-return path is always the right default; there's no call-site that prefers abort-on-OOM |

## Consequences

- **Positive**:
  - Netflix#1420 reproducer resolved: a second concurrent
    VMAF-CUDA process that OOMs on `cuMemAlloc` now gets a
    clean `-ENOMEM` return from `vmaf_cuda_buffer_alloc` (and
    transitively from `init_fex_cuda`), instead of aborting.
  - NDEBUG footgun eliminated: with `assert(0)` gone, release
    builds can no longer silently continue past a failed
    `cuMemAlloc` into a segfault.
  - Downstream integrations (ffmpeg filter, libvmaf CLI,
    mcp-server) can distinguish transient resource pressure
    (`-ENOMEM`, `-EIO`) from configuration errors (`-EINVAL`)
    and device loss (`-ENODEV`). Enables "retry with smaller
    batch", "fall back to CPU", or "surface a clean error"
    behaviour.
  - Every error path now logs via `fprintf(stderr, "CUDA
    error at %s:%d: %s (%d) in %s\n", ...)` including the
    `#CALL` stringification — more actionable than the old
    `printf` + `assert(0)` stack-trace dump.
  - ADR-0122 / ADR-0123 null-guards on the public entry
    points in `common.c` are preserved verbatim; the
    graceful-return path and the null-guard path compose
    cleanly.
- **Negative**:
  - **Visible behaviour change** for callers that relied on
    the process aborting rather than returning an error —
    specifically, if any downstream code *assumed* CUDA
    failures produced a visible crash (and therefore treated
    VMAF-CUDA success as "process still alive"), that
    assumption is now false. Flagged under `### Changed` in
    CHANGELOG.
  - Larger binary: every touched function now has an extra
    cleanup label + error-translation switch. Measured
    delta on `libvmaf.so.3.0.0`: ~4 kB code size increase
    (0.05%), within noise.
  - Twelve `void → int` signature changes in `static`
    helper functions. Private to the CUDA TUs; no public
    ABI change.
- **Neutral / follow-ups**:
  - New reducer test
    [`core/test/test_cuda_buffer_alloc_oom.c`](../../core/test/test_cuda_buffer_alloc_oom.c)
    exercises `cuMemAlloc(1 TiB)`, verifies the return is
    `-ENOMEM` (not abort). GPU-gated — runs only when CUDA is
    available at test time; skips cleanly otherwise.
    Verified on this host to actually hit the OOM path at
    `cuMemAlloc` (not NULL-state early-return).
  - Rebase-notes entry 0049 pins the invariant: on upstream
    sync, keep the fork's `CHECK_CUDA_GOTO` /
    `CHECK_CUDA_RETURN` macros — upstream Netflix still
    uses `assert(0)` in the macro.
  - Pre-existing `performance-no-int-to-ptr` warnings in
    `integer_adm_cuda.c` + `integer_vif_cuda.c` (CUDA
    device-pointer casts `(T*)(size_t)cu_ptr.data` — inherent
    to the CUDA Driver API) bracketed with
    `NOLINTBEGIN/END(performance-no-int-to-ptr)` + inline
    ADR-0141 citation. Two scoped blocks + one
    `NOLINTNEXTLINE` cover 47 sites.

## Verification

- `meson test -C core/build-cuda` → **39/39 pass** (was:
  38/38 pre-PR; + new `test_cuda_buffer_alloc_oom`).
- `meson test -C build` (CPU-only) → **35/35 pass**.
- Reducer test verified to hit the OOM branch on this host:
  `cuMemAlloc(1 TiB)` returns `CUDA_ERROR_OUT_OF_MEMORY (2)`
  → `vmaf_cuda_result_to_errno(2) = -ENOMEM` → caller
  receives `-ENOMEM`, process continues. Pre-fix, the same
  line fired `assert(0)` and aborted.
- `clang-tidy -p core/build-cuda --quiet <6 files>` →
  **exit 0** on every file (zero errors in the
  `WarningsAsErrors` set). NOLINT brackets added for
  `performance-no-int-to-ptr` at 47 CUDA device-pointer-cast
  sites, each with inline ADR-0141 upstream-parity citation.
- `pre-commit run --files <touched>` → all hooks pass.
- ADR-0122 / ADR-0123 null-guards (`is_cudastate_empty(...)`
  at `vmaf_cuda_sync`, `vmaf_cuda_release`,
  `vmaf_cuda_buffer_alloc`, `vmaf_cuda_buffer_free`,
  `vmaf_cuda_buffer_upload`, `vmaf_cuda_buffer_download`,
  `vmaf_cuda_buffer_host_alloc`, `vmaf_cuda_buffer_host_free`)
  preserved verbatim.

## References

- Upstream issue:
  [Netflix/vmaf#1420](https://github.com/Netflix/vmaf/issues/1420)
  ("Crash when 2 files are analyzed simultaneously
  .../src/cuda/common.c:166: vmaf_cuda_buffer_alloc:
  Assertion `0' failed."), OPEN as of 2026-04-24.
- [ADR-0122](0122-cuda-framesync-segfault-hardening.md) —
  fork PR #60 CUDA framesync hardening.
- [ADR-0123](0123-cuda-null-guard.md) — fork PR #62 null-guard
  at CUDA public entry points.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (scoping the `performance-no-int-to-ptr`
  NOLINTs).
- [ADR-0154](0154-score-pooled-eagain-netflix-755.md) — prior
  precedent for "transient vs fatal" error-code splits in
  libvmaf.
- [rebase-notes 0049](../rebase-notes.md) — upstream-sync
  invariants for this decision.
- Backlog: `.workingdir2/BACKLOG.md` T1-6.
- User direction 2026-04-24 popup: "T1-6 CUDA concurrency
  assert Netflix#1420" → "Wholesale: replace CHECK_CUDA
  everywhere".
