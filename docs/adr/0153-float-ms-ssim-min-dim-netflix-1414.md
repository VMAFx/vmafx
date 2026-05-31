# ADR-0153: `float_ms_ssim` init rejects input below 176×176

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: correctness, ms-ssim, netflix-upstream

## Context

Netflix upstream issue
[#1414](https://github.com/Netflix/vmaf/issues/1414) reports:

> If we use a QCIF resolution video, the `float_ms_ssim` feature
> throws an error saying `error: scale below 1x1!` …

The cause is that MS-SSIM is defined as a **5-level** Gaussian
pyramid with an **11-tap** filter (constants `SCALES = 5` and
`GAUSSIAN_LEN = 11` in
[`core/src/feature/iqa/ssim_tools.h`](../../core/src/feature/iqa/ssim_tools.h)).
Each level down-samples the image by 2×. For the 11-tap Gaussian
to still fit at level 4 (the deepest scale), the input must
satisfy `min(w, h) >= GAUSSIAN_LEN << (SCALES - 1) = 11 << 4 =
176`. Below that, the pyramid walks off the kernel footprint at
a mid-level scale and
[`ms_ssim_check_scale_ok`](../../core/src/feature/ms_ssim.c)
emits `"error: scale below 1x1!\n"` mid-run, followed by a
cascading `"problem reading pictures"` / `"problem flushing
context"` that makes the real failure hard to find in tooling
output.

Fork state pre-this-PR matches upstream: the scale check exists
but only fires deep inside `compute_ms_ssim`, after the
extractor has already been initialised and the first frame has
begun processing. No init-time guard.

## Decision

Reject small resolutions up front at init time.

1. In
   [`core/src/feature/float_ms_ssim.c`](../../core/src/feature/float_ms_ssim.c),
   compute the minimum supported dimension from the existing
   `GAUSSIAN_LEN` + `SCALES` constants:

   ```c
   const unsigned min_dim = GAUSSIAN_LEN << (SCALES - 1); /* 176 */
   if (w < min_dim || h < min_dim) {
       vmaf_log(VMAF_LOG_LEVEL_ERROR,
                "%s: input resolution %ux%u is too small; the %d-level "
                "%d-tap MS-SSIM pyramid requires at least %ux%u "
                "(Netflix#1414)\n",
                fex->name, w, h, SCALES, GAUSSIAN_LEN, min_dim, min_dim);
       return -EINVAL;
   }
   ```

2. Extract the SIMD-dispatch wiring into a new static helper
   `ms_ssim_init_simd_dispatch` to keep the `init` function body
   under the ADR-0141 60-line `readability-function-size` limit
   after the new 12-line guard block is added.
3. Unit test:
   [`core/test/test_float_ms_ssim_min_dim.c`](../../core/test/test_float_ms_ssim_min_dim.c),
   registered in
   [`core/test/meson.build`](../../core/test/meson.build).
   Three subtests — registration, reject below minimum (5
   boundary cases: 160×144, 160×200, 200×160, 175×176, 176×175),
   and accept at/above the minimum (176×176 exact, 576×324).

The minimum dimension is computed from the filter constants so
it stays in sync if upstream ever changes `SCALES` or
`GAUSSIAN_LEN`; no magic `176` in the source.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Dynamically reduce pyramid levels for small inputs** | Supports all resolutions | The 5-level pyramid and per-level weights are part of the MS-SSIM *definition*; computing with fewer levels produces a *different metric* that is not MS-SSIM. Results would be numerically incomparable with every other tool that computes MS-SSIM | Rejected — misrepresenting the metric is worse than refusing the call |
| **Switch small-input runs to plain SSIM silently** | User gets *some* score | Silent metric substitution is the worst of all worlds; breaks the user's assumption about what column they're reading | Rejected — silent substitution is a correctness bug, not a user feature |
| **Keep deep-pyramid error; improve the cascading error message only** | Smallest diff | The first-frame-in failure is still confusing (user sees extractor failure *plus* pipeline cascade); init-time rejection is strictly cleaner | Rejected — the issue reporter explicitly asked for an init-time decision |
| **Reject at a higher resolution floor (e.g. 256×256)** | Safety margin against future kernel enlargement | Excludes valid input where the math works cleanly; 176×176 is the mathematical floor | Rejected — use the derived minimum, not an arbitrary round number |

## Consequences

- **Positive**:
  - QCIF / sub-QCIF / small-tile users get one clear error at
    `vmaf_feature_extractor_context_create` / `init` time instead
    of a confusing mid-run cascade. Diagnostic includes the
    input resolution, the required minimum, and a Netflix#1414
    pointer.
  - The error message is specific enough that a user running
    just `--feature float_ms_ssim` on a small input knows
    immediately which feature to drop and what the fix is
    (upscale, use `float_ssim` instead).
  - Zero behaviour change for inputs ≥ 176×176; the guard is a
    strict early-exit.
- **Negative**:
  - **Visible behaviour change at the API boundary**: pre-fix,
    `float_ms_ssim_init` succeeded on a small input and only
    failed mid-frame; post-fix, `init` fails immediately with
    `-EINVAL`. Callers that relied on init succeeding (even if
    the eventual extraction was broken) now see the failure
    earlier. This is strictly an improvement, but is visible
    and goes under CHANGELOG `### Fixed`.
- **Neutral / follow-ups**:
  - The `compute_ms_ssim` deep-pyramid guard at
    `ms_ssim_check_scale_ok` is now dead code in normal flow
    (init rejects before the first frame). Kept as defensive
    belt-and-braces — in case a caller ever bypasses init
    (e.g., direct `compute_ms_ssim` call from a future test
    harness).
  - Upstream Netflix#1414 is still OPEN. If upstream lands a
    similar fix later, the fork's version should merge cleanly
    on `/sync-upstream` — the guard sits in the fork's own
    `init` function. Same-named `min_dim` / error-message
    wording might differ; keep the fork's version on conflict
    unless upstream's is strictly more informative.

## Verification

- `meson test -C build` → **34/34 pass** (was 33; one new test
  file added).
- `meson test -C build test_float_ms_ssim_min_dim` → 3/3
  subtests pass:
  - `test_float_ms_ssim_is_registered`
  - `test_float_ms_ssim_init_rejects_below_min_dim`
  - `test_float_ms_ssim_init_accepts_min_dim`
- **Reducer verified**: `git stash push
  core/src/feature/float_ms_ssim.c && ninja -C build &&
  meson test -C build test_float_ms_ssim_min_dim` reports
  `Fail: 1` — the test is a real gate, not a tautology.
- Reproducer from the upstream issue (with a 160×144 YUV):

  ```text
  libvmaf ERROR float_ms_ssim: input resolution 160x144 is too small;
  the 5-level 11-tap MS-SSIM pyramid requires at least 176x176 (Netflix#1414)
  ```

- `clang-tidy -p build core/src/feature/float_ms_ssim.c` →
  zero warnings (`init` stays within the
  `readability-function-size` budget via the extracted
  `ms_ssim_init_simd_dispatch` helper).

## References

- Upstream issue:
  [Netflix/vmaf#1414](https://github.com/Netflix/vmaf/issues/1414)
  ("float_ms_ssim broken for videos < 176x176 (eg QCIF)"), OPEN
  as of 2026-04-24.
- Backlog: `.workingdir2/BACKLOG.md` T1-4.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — the touched-
  file lint-clean rule that drove the SIMD-dispatch helper
  extraction.
- User direction 2026-04-24 popup: "T1-4 float_ms_ssim broken
  <176x176 QCIF (Netflix#1414)".
