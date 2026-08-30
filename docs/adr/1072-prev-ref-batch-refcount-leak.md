<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1072: Fix PREV_REF refcount leak in threaded batch and serial dispatch paths

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `core`, `threading`, `memory`, `picture-pool`, `bug`

## Context

The `VMAF_FEATURE_EXTRACTOR_PREV_REF` protocol (introduced in PR #688) lets
temporal extractors receive the previous frame via `fex->prev_ref` rather than
managing their own internal state with `VMAF_FEATURE_EXTRACTOR_TEMPORAL`.
`feature_extractor.cpp::vmaf_feature_extractor_context_extract()` implements a
SWAP on success: it unrefs the old `fex->prev_ref` and calls `vmaf_picture_ref`
to store the current frame with a bumped refcount.

Both dispatch paths that feed PREV_REF extractors copied `vmaf->prev_ref` into
`fex->prev_ref` via a bare struct copy (no `vmaf_picture_ref`), ran the
extractor, then cleared `fex->prev_ref` with a bare `memset` — without first
calling `vmaf_picture_unref`.  After the SWAP, `fex->prev_ref` holds the
current frame with a bumped refcount; the bare memset discards that counted
reference without triggering the pool-release callback.

The net effect: every frame processed by a PREV_REF extractor leaks one
picture-pool slot.  After `~pool_size` frames the pool is exhausted and
`vmaf_picture_pool_fetch()` blocks in `pthread_cond_wait` indefinitely.

The paths affected:

- **`threaded_extract_batch_func`** (master active path for n_threads > 0):
  struct copy at line 1683 + bare memset at line 1690.  Additionally, the
  SWAP's unref drops the old-frame VmafRef to 0 and frees it; the subsequent
  `vmaf_picture_unref(&f->prev_ref)` in the `unref:` block is then a
  use-after-free.

- **`threaded_extract_func`** (dead code, kept for consistency):
  same struct copy + bare memset pattern.

- **`read_pictures_dispatch_one`** (serial path, n_threads=0): was already
  fixed in PR #741 via ADR-0778 (uses `vmaf_picture_ref`, not struct copy).

Three CI test failures share this root cause: `test_picture_pool_basic`
deadlocks because `integer_motion` is PREV_REF and is registered via
`vmaf_use_features_from_model("vmaf_v0.6.1")` with n_threads=4.

Two additional CI failures were separate test-design bugs:

- `test_float_ms_ssim_cpu_cuda_parity` and `test_hip_ms_ssim_parity` used a
  256x144 fixture; `float_ms_ssim` requires `min(w,h) >= 176` for its
  5-level 11-tap Gaussian pyramid (see `float_ms_ssim.c:131`) and returned
  `-EINVAL` on the first `vmaf_read_pictures` call.

- `test_hip_motion_parity` queried `VMAF_integer_feature_motion_score` which
  `integer_motion` only writes when `debug=true`; without that option the
  extractor emits only `VMAF_integer_feature_motion_sad_score`.

## Decision

In `threaded_extract_batch_func`: after `vmaf_feature_extractor_context_extract`
returns, if the extractor carries `VMAF_FEATURE_EXTRACTOR_PREV_REF`, call
`vmaf_picture_unref(&td->fex_ctx[i]->fex->prev_ref)` before the memset to
balance the SWAP's bump (on success) or the struct-copy's share (on error).
Then zero `f->prev_ref` to prevent the `unref:` block from double-freeing the
now-consumed VmafRef.

Apply the same one-unref-before-memset pattern to `threaded_extract_func` for
consistency (dead code path, but must not carry a latent bug if re-activated).

Fix the test-design bugs:

- Raise fixture height from 144 to 192 in both ms_ssim parity tests.
- Pass `debug=1` via `vmaf_feature_dictionary_set` to `vmaf_use_feature` in
  `run_cpu_motion` so `motion_score` is emitted on the CPU path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Use `vmaf_picture_ref` instead of struct copy at dispatch (match serial path) | Eliminates shared-pointer aliasing; simpler ownership | Adds one ref/unref per frame per PREV_REF extractor; requires zeroing fex->prev_ref after extract separately | The fix is correct with the minimal unref-before-memset change; converting to full ref would be a larger refactor with no observable correctness advantage |
| Disable PREV_REF flag on integer_motion in the batch path | Avoids the code path entirely | Breaks temporal correctness of motion scores when threaded | Not an option; motion requires the previous frame |
| Disable the 4 failing sub-tests | Avoids touching libvmaf.c | Hides a real memory-safety bug; pool exhaustion will reappear in other tests | The root cause is a one-line fix per path |
| Fix only the batch path, leave threaded_extract_func with the bug | Minimal change | Dead code with a latent bug is a maintenance hazard | Cheap to fix both while the code is open |

## Consequences

- **Positive**: picture-pool exhaustion from PREV_REF extractors in threaded
  mode is eliminated.  Tests that previously deadlocked or returned EINVAL
  now pass.  The serial path (ADR-0778) and the batch path are now consistent.
- **Negative**: none.
- **Neutral / follow-ups**: `threaded_extract_func` and `threaded_read_pictures`
  remain dead code; a follow-up cleanup PR can remove them.

## References

- ADR-0778: serial-path PREV_REF fix (PR #741)
- ADR-0795: batch-threading thread-safety analysis
- PR #688: integer_motion TEMPORAL → PREV_REF migration
- PR #764: pattern for disabling sub-tests (reference for the alternative not taken)
- `float_ms_ssim.c:131`: minimum-dimension check comment and rationale
