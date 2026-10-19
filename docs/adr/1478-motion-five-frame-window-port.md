<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1478: Port `motion_five_frame_window` from Netflix; the deferral of ADR-0337 ends for this option

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1491](1491-gpu-motion-five-frame-window.md) for decision 5 (GPU twins leave motion_five_frame_window to the CPU))
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `upstream-port`, `motion`, `feature-extractor`, `models`, `picture-pool`, `gpu-parity`, `golden-gate`, `rc3`

## Context

Netflix/vmaf has a second temporal window for the integer motion feature
(`a2b59b77`, 2026-05-08, kept through the rewrite `a4a1492d`): with
`motion_five_frame_window=true` the SAD of frame `n` is taken against frame
`n-2` instead of `n-1`, and `motion2` of frame `n` is the smaller of the SADs
of frames `n-1` and `n+1`. The four shipped models
`model/vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_*.json` set the option.

The fork declared the option and refused it:
[ADR-0337](0337-motion-v2-public-api-options.md) made `motion_v2` return
`-ENOTSUP` at `init()` and [ADR-0994](0994-coverage-build-fix-motion-v2-ref.md)
did the same for `motion`, because the frame `n-2` reaches an extractor
through a second field on `VmafFeatureExtractor` (`prev_prev_ref`) and through
`vmaf_read_pictures()`, which the fork had decomposed
([ADR-0152](0152-vmaf-read-pictures-monotonic-index.md)) and given its own
picture-ownership rules ([ADR-0778](0778-picture-pool-framesync-audit.md),
[ADR-1431](1431-read-pictures-owns-pictures-on-every-return.md)). ADR-0337
deferred that plumbing "to a follow-up PR"; none came. The cost by October
2026: the four `_hfr` models could not be scored on the fork, and 13 Netflix
golden tests carried a fork-added `@unittest.skip` (nine in
`python/test/feature_extractor_test.py`, four in
`python/test/vmaf_v1_quality_runner_test.py`).

The upstream parity audit of 2026-10-02 listed the option as a missing port,
and the maintainer decided that inherited code follows Netflix's source, with
deviations only by ADR, and that this option is ported now, on the CPU and on
the GPU twins.

Upstream no longer has `motion_v2`: `a4a1492d` replaced `integer_motion.c`
with the pipelined implementation and deleted `integer_motion_v2.c`. The fork
keeps both extractors, with the same SAD pipeline.

Upstream also makes every run keep the reference picture of frame `n-2`,
whether or not an extractor reads it, and raises its picture-pool sizes to
match. A review of the first revision of this port, which did the same with a
warning for small pools, found that a preallocated pool of three pictures, enough
for a serial run until then, would stall on the third frame: a hang for an
existing API caller that never sets the option.

## Decision

The fork computes `motion_five_frame_window`.

1. **`motion` runs upstream's statements.** `integer_motion.c::extract()`
   takes the SAD against `fex->prev_prev_ref` when the option is set
   (`min_idx = 2`), and the `-ENOTSUP` guard is gone. The flush is upstream's,
   as before.
2. **`motion_v2` does the same**, as upstream's last `integer_motion_v2.c`
   (`a4a1492d^`) did. The two extractors keep their two option tables
   (ADR-0337, alternative A1), but the derivation of `motion2` and `motion3`
   from the SAD scores exists once: `vmaf_motion_window_flush()` in
   `integer_motion.c`, declared in `core/src/feature/motion_window.h`, called
   by both.
3. **The framework hands a PREV_REF extractor frame `n-1`, and frame `n-2`
   to an extractor that reads it.** `VmafFeatureExtractor` gains
   `reads_prev_prev_ref()`, which `motion` and `motion_v2` answer with the
   option. The context keeps `prev_prev_ref`, and rotates the two pictures
   per frame as upstream does, only while a registered extractor answers true
   (`admit_prev_prev_ref()` in `core/src/libvmaf.c`); otherwise it keeps
   frame `n-1` alone, exactly as before the port. This is a deliberate
   deviation from Netflix, which keeps frame `n-2` in every run. It changes
   which pictures the context holds, not what any extractor computes; the
   proof is under Consequences. The fork's ownership rule stays: an extractor
   gets counted references of its own (`fex_take_prev_refs()` /
   `fex_release_prev_ref()`), where upstream copies the structs.
4. **A pool that would stall is refused, never left to wait.** While frame
   `n-2` is kept, a preallocated pool needs at least four pictures (the two
   kept reference pictures and the current pair). Whichever comes second,
   `vmaf_preallocate_pictures()` or the registration of the extractor
   (`vmaf_use_feature()`, `vmaf_use_features_from_model()`, and the
   first-frame CPU fallback of
   [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md)), returns
   `-EINVAL` with one error line naming `pic_cnt` and the minimum. Without
   such an extractor the pool behaves as before the port: no minimum, and the
   default pool stays `n_threads * 2` (upstream's `n_threads * 2 + 2` while
   frame `n-2` is kept). The CLI sizes its pool before it loads the models,
   so a serial run takes four pictures, one more than it needs without the
   window; a threaded run keeps its count, which is also upstream's.
5. **GPU twins leave the option to the CPU until they have the window.**
   `motion_cuda`, `motion_sycl` and `motion_hip` mark the option
   `VMAF_OPT_FLAG_DEFAULT_ONLY` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)),
   so a model or a `--feature motion` that sets it is computed by the CPU
   extractor on every backend; the Metal twin and the four `motion_v2` twins
   do not declare the option, with the same effect
   ([ADR-1359](1359-cli-feature-backend-twin.md)). A twin named directly with
   the option still fails (`-ENOTSUP`, or "unknown option"). The device
   implementations follow per backend.
6. **The 13 skip markers are removed.** No assertion changes.

Statements of earlier ADRs this replaces: ADR-0337 §Decision,
"`motion_five_frame_window=true` is rejected at `init()` with `-ENOTSUP`",
and its deferral of the picture-pool plumbing; ADR-0994 decision 1 (the guard
in `integer_motion.c`). The rest of both stands, including ADR-0337's choice
of duplicate option tables.
[ADR-0219](0219-motion3-gpu-coverage.md)'s `-ENOTSUP` for a GPU twin stands
until that twin has the window.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the deferral | No framework change | Four shipped models unusable, 13 golden tests skipped, a known difference from Netflix with no reason behind it | Maintainer decision: port now |
| The extractor keeps its own copy of frame `n-2` | No change to `vmaf_read_pictures()` | With worker threads each worker has a private extractor and sees an arbitrary subset of frames, so no extractor can hold "the frame two back"; the framework is the only place that sees frames in order | Does not work with `--threads` |
| Keep frame `n-2` in every run, as upstream does, and warn about a pool below four (this ADR's first revision) | Upstream's code; no mechanism of the fork's own | A preallocated pool of three, which served a serial API caller until now, stalls on the third frame; a warning does not make that additive for an existing caller (HISS-14) | **Frame `n-2` only for an extractor that reads it**, a pool too small for it refused with `-EINVAL` |
| The same, as a breaking change (`!` and a `Migration:` footer) | Upstream's code | Every existing caller with a pool of three has to change for a picture it never uses | Not needed: the conditional window moves no score |
| Port to `motion` only, `motion_v2` keeps `-ENOTSUP` | Smaller diff | Two extractors with one option table and different behaviour for the same option | Both, through one window function |
| A copy of the five-frame flush in `motion_v2` and in every twin | Each file reads on its own | The fork already carries one copy of the three-frame flush per twin (`T-GPU-CUDA-HIP-DUPLICATED-KERNELS-2026-10-02`); a second window would double them | **One definition**, `vmaf_motion_window_flush()` |
| Implement the twins in the same pull request | One step | Three devices to verify before the CPU port, the models and the golden tests can land | CPU first with a correct fallback; twins stacked per backend |

## Consequences

- **Positive**:
  - `motion` with the option equals Netflix `9e48141b` bit for bit: 2232
    harness runs (31 fixtures, 8 option sets, scalar / AVX2 / default
    dispatch, serial and 1 and 4 worker threads), 95 130 values at `%.17g`,
    0 differences.
  - The four `vmaf_v1.0.16_hfr_*` models score. Against Netflix `9e48141b`
    on eight clips, three dispatch modes and three thread modes (288 runs),
    per frame: their motion features are identical (22 032 of 22 032 values),
    `adm` and `cambi` too (58 752 of 58 752), and the model score differs on
    5436 of 7344 frames by at most 2.5e-5 because `speed_chroma` differs (by
    at most 3.5e-5), which the non-HFR models share and another pull request
    of the parity work owns. On the audit's converged tree (the fork with
    every other difference from Netflix undone) with this port applied, all
    119 088 values of those 288 runs are identical, the model scores
    included.
  - Netflix golden gate (x86): 280 passed, 3 skipped (271 and 12 before);
    the v1 model file 9 passed, 0 skipped (5 and 4 before). On the aarch64
    cross build under qemu-user the 13 tests pass as well.
  - With `--backend cuda`, `sycl` or `hip` an `_hfr` model scores, its motion
    feature on the CPU.
  - Keeping frame `n-2` only for a reader moves no score. With the
    conditional window, against Netflix `9e48141b`: `motion` with the option
    and with the same eight option sets without it, NUM_FEATURES; the four
    `_hfr` models and the four SDR `vmaf_v1.0.16` models, NUM_MODELS; on the
    audit's converged tree, NUM_UND5.
  - Without an extractor that reads frame `n-2`, a context holds the pictures
    it held before the port and a pool of three serves a whole sequence
    (`test_pool_of_three_without_the_window`, serial and with a worker).
- **Negative**:
  - With the option the context keeps one more reference picture alive (one
    frame of memory), as upstream does in every run.
  - An API caller that registers a five-frame extractor (an `_hfr` model)
    next to a preallocated pool below four pictures gets `-EINVAL` and has to
    make the pool four. Callers that allocate each picture with
    `vmaf_picture_alloc()` (the FFmpeg filters) are not affected. The CLI
    preallocates four pictures in a serial run, one more than before.
  - `reads_prev_prev_ref()` and the admission check are a mechanism upstream
    does not have; a sync keeps them
    (`docs/development/rebase-sensitive-invariants.md`).
- **Neutral / follow-ups**:
  - Device implementations of the window for the CUDA, SYCL and HIP twins of
    `motion` and `motion_v2`, each bit-identical to the CPU, with a parity
    gate cell that sets the option
    (`T-GPU-MOTION-FIVE-FRAME-WINDOW-2026-10-02`). The Metal twins keep the
    CPU fallback: no device to measure on.
  - Whether the fork keeps `motion_v2` as a second extractor at all, now that
    upstream has one, is a deduplication question (RC5,
    `T-MOTION-V2-SECOND-EXTRACTOR-2026-10-03`) and is not decided
    here. What this ADR settles of ADR-0337's duplicate-surface concern is the
    behaviour: one window function. The two option tables remain.
  - Upstream's own model documentation describes the window as "frames i-2,
    i, i+2"; the code, and the golden assertions that pin it, read frames
    `n-3`, `n-1` and `n+1` for `motion2` of frame `n`. The fork follows the
    code and documents what it computes (`docs/metrics/motion.md`).

## References

- `Q` (popup answer of the maintainer, 2026-10-02): "Port now, CPU and twins
  (Recommended)".
- `req` (popup answer, 2026-10-02): "Netflix's source, deviations only by
  ADR".
- Review of #1887 before merge (2026-10-03): the unconditional window of the
  first revision made a preallocated pool of three a hang for existing API
  callers; this revision keeps frame `n-2` only for a reader and refuses a
  pool too small for it.
- Netflix/vmaf [`a2b59b77`](https://github.com/Netflix/vmaf/commit/a2b59b77)
  (`libvmaf/motion_v2: add motion_five_frame_window`),
  [`4e469601`](https://github.com/Netflix/vmaf/commit/4e469601),
  [`a4a1492d`](https://github.com/Netflix/vmaf/commit/a4a1492d)
  (`libvmaf: replace integer_motion with pipelined v2 variant, rename`);
  compared against master `9e48141b`.
- [ADR-0337](0337-motion-v2-public-api-options.md),
  [ADR-0994](0994-coverage-build-fix-motion-v2-ref.md),
  [ADR-0219](0219-motion3-gpu-coverage.md),
  [ADR-0152](0152-vmaf-read-pictures-monotonic-index.md),
  [ADR-0778](0778-picture-pool-framesync-audit.md),
  [ADR-1431](1431-read-pictures-owns-pictures-on-every-return.md),
  [ADR-1316](1316-gpu-option-value-capability-fallback.md),
  [ADR-1359](1359-cli-feature-backend-twin.md),
  [ADR-0024](0024-netflix-golden-preserved.md),
  [ADR-1317](1317-golden-gate-build-isolation.md),
  [ADR-1461](1461-strict-fp-every-translation-unit.md).
