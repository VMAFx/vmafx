<!-- markdownlint-disable MD013 MD060 -->
# Research digest — c-rework-vif-motion: bit-exact rework of the scalar VIF and float-motion TUs (2026-09-02)

**Unit:** `c-rework-vif-motion` (wave 1 of the "rework upstream code to fork
standards" campaign, clang-tidy debt baseline of 2026-08-31).
**Files:** `core/src/feature/integer_vif.c` (890 LOC), `core/src/feature/vif_tools.c`
(777 LOC), `core/src/feature/float_motion.c` (582 LOC), plus a one-word `const`
in `core/src/feature/integer_vif.h`. All keep the Netflix header.
**Constraint:** the three Netflix golden pairs are the only numerical ground
truth; `vif_neon` / `vif_avx2` / `vif_avx512` are tested against this scalar
code and share `vif_compute_line_residuals`, whose numeric path must not change.

## 1. Findings discharged

Measured with `clang-tidy -p build --quiet <file>` (LLVM 22.1.8, CPU-only
`meson setup build core -Denable_cuda=false -Denable_sycl=false`, gcc 16.2.1)
and `cppcheck 2.21.1` with the `make lint-c` flags.

| File | clang-tidy before | clang-tidy after | Checks discharged |
| --- | --- | --- | --- |
| `integer_vif.c` | 14 | 0 | `readability-function-size` ×5 (`vif_statistic_8`, `vif_statistic_16`, `vif_compute_line_residuals`, `init`, `write_scores`), `bugprone-implicit-widening-of-multiplication-result` ×4, `modernize-use-nullptr` ×2, `readability-isolate-declaration`, `performance-type-promotion-in-math-fn` (`round`), `misc-use-internal-linkage` |
| `vif_tools.c` | 26 | 0 | `readability-isolate-declaration` ×17, `performance-type-promotion-in-math-fn` ×5 (`ceil`, `floor` ×4), `readability-function-size` ×4 (`vif_statistic_s`, `vif_filter1d_s`, `vif_filter1d_sq_s`, `vif_filter1d_xy_s`) |
| `float_motion.c` | 5 (+3 cited `readability-function-size` NOLINTs) | 0 (0 NOLINTs) | `modernize-use-nullptr` ×3, `readability-braces-around-statements`, `misc-use-internal-linkage`; the `init` / `extract` / `close` NOLINTs from the Netflix b949cebf port are discharged by refactor |

cppcheck: `integer_vif.c` lost its `constVariablePointer` ×4,
`constParameterPointer` (`vif_compute_line_residuals`'s `s`) and a
branch-limited `knownConditionTrueFalse`; `vif_tools.c` lost 22 `variableScope`
findings (all from the multi-declarations); `float_motion.c` lost the redundant
`else if (s->index == 0)` (`knownConditionTrueFalse`). One cited
`cppcheck-suppress constParameterCallback` remains on `flush()` — the
prototype is fixed by the `VmafFeatureExtractor.flush` callback type. The
whole-program `unusedFunction` rows on exported entry points are the
pre-existing configuration artefact documented by the c-rework-core unit.

## 2. Refactor map

| Original function | Now | Notes |
| --- | --- | --- |
| `vif_statistic_8` / `vif_statistic_16` / `vif_compute_line_residuals` (three verbatim copies of the horizontal pass + per-pixel statistic) | `vif_horizontal_pixel` (5 moments of one column), `vif_accumulate_pixel` (log-domain / non-log accumulation into `VifResiduals`), `vif_store_residuals` (num / den), plus per-bit-depth `vif_vertical_line_8` / `vif_vertical_line_16` and `vif_shift_for_scale` | `FORCE_INLINE`; expressions, types and evaluation order verbatim. The three public entry points are loops over the helpers. |
| `init` (integer VIF) | `vif_init_dispatch` (SIMD function-pointer selection) + `vif_buffers_alloc` (single-allocation byte-cursor layout) | `goto fail` removed; the dictionary-failure path frees the buffer and NULLs `buf.data`. |
| `write_scores` | `write_scale_scores` + `write_debug_scores` over `vif_scale_{score,num,den}_names[4]` | Append order unchanged (scale scores, totals, then num/den per scale). The `double` sums stay as explicit left-to-right expressions, not loops. |
| `decimate_and_pad` | unchanged shape | `(ptrdiff_t)i * 2` / `(ptrdiff_t)j * 2` replace the implicitly-widened `unsigned` products. |
| `vif_statistic_s` (float) | `vif_pixel_statistic_s` (per-pixel matching_matlab statistic) | The upstream `matching_c` comment block moved to file scope above the helper. |
| `vif_filter1d_s` / `_sq_s` / `_xy_s` | `vif_use_avx2_convolution` (ADR-0504 dispatch), `vif_mirror_index` (reflect-101), `vif_filter1d_vertical_s` / `_vertical_sq_s` / `_vertical_xy_s`, shared `vif_filter1d_horizontal_s` | Scratch-row `aligned_malloc` failure now logs and returns instead of dereferencing NULL. |
| `float_motion` `init` / `extract` / `close` | `MotionPlane plane[3]` (Y, U, V) with `motion_chroma_heights`, `motion_plane_alloc` / `motion_plane_free` / `motion_free_planes`, `motion_check_min_dim`, `motion_select_sad_line`, `motion_append`, `motion_clip` / `motion_blend_clip`, `motion_append_forced_zero`, `motion_blur_plane`, `motion_copy_and_blur`, `motion_score_pair` | The chroma-height check runs before any allocation (fixes the Y-buffer leak on `-EINVAL`); the `switch` has a `default`. |

## 3. Why helper extraction is bit-exact here

- The build compiles C with `-std=c23 -O3` and **no** `-march`: ISO mode sets
  `-ffp-contract=off`, and the baseline x86-64 target has no FMA, so there is
  no contraction opportunity that moving an expression into an
  `always_inline` helper could change. `FLT_EVAL_METHOD == 0` on SSE, so
  `float` and `double` expressions evaluate in their own precision.
- The integer VIF statistic is `uint32` / `uint64` / `int64` arithmetic with
  one `double` island (`g`, `sv_sq`, `numer1_tmp`); every expression in
  `vif_accumulate_pixel` is character-for-character the upstream one, with the
  same operand types.
- `roundf(x)` for `x = log2f(n) * 2048 < 2^15` returns the same integer as
  `round((double)x)`: the promotion is exact and the rounded result is
  representable in `float`. Proven exhaustively over all 32768 LUT entries at
  `-O0` and `-O3` (0 mismatches). `ceilf` / `floorf` on a `float` operand are
  likewise identical to the promoted `ceil` / `floor` for the `int`-converted
  results these call sites consume.
- `MIN(score * w, max)` evaluated through a helper returning `double` is the
  same IEEE value as the macro in an argument position (no x87 extended
  precision on x86-64).
- The `double` sums in `write_debug_scores` and `motion_score_pair` keep the
  original left-to-right association (Y, then U, then V).

## 4. Verification

### 4.1 Netflix golden pairs (CLI form `python/test/vmafexec_test.py` builds)

`build/tools/vmaf` from the reworked tree, `--precision max`:

| Pair | Measured `vmaf` mean | Golden assertion (`places=4`) | Source |
| --- | --- | --- | --- |
| `src01_hrc00` vs `src01_hrc01` (576x324, `vmaf_v0.6.1` + float psnr/ssim/ms_ssim) | 76.66783086300072 | 76.66783025 | `vmafexec_test.py::test_run_vmafexec_runner_matched_to_vmafossexec` |
| `checkerboard_..._0_0` vs `..._1_0` (1920x1080) | 35.0686714193046 | 35.06866714286451 | `quality_runner_test.py::test_run_vmaf_runner_checkerboard` |
| `checkerboard_..._0_0` vs `..._10_0` (1920x1080) | 7.985899011514694 | 7.985898744818505 | `quality_runner_test.py::test_run_vmaf_runner_checkerboard` |
| `src01_hrc00` vs `src01_hrc01`, `vmaf_float_v0.6.1` (float VIF + float motion) | 76.66744002507649 | 76.66740433333332 | `vmafexec_test.py::test_run_vmafexec_runner_float_fex` |

### 4.2 Pre-change vs post-change byte diff (31 cases)

The pre-change `vmaf` + `libvmaf.so.3` were stashed before any edit and both
binaries were run through the same 31-case matrix (`--precision max --json`),
diffing every line except the `"fps"` timing line and the `"version"`
git-describe string. **31 / 31 identical.** (A repeat run after the rebase onto
the advanced `master` was again 31 / 31 identical when the parsed JSON is
compared; textually, two `motion_add_scale1` cases moved the `motion3_mdc` key
within a frame's metrics object — that key is appended at `index - 1` from a
later frame, so its insertion order under `--threads 8` depends on thread
scheduling. Values are unchanged; the ordering artefact predates this PR.) The
matrix covers:

- the three golden pairs with the integer and the float model;
- `--cpumask 0` (scalar `vif_statistic_8/16`, scalar `subsample_rd_8/16`,
  scalar float VIF filters and statistic, scalar SAD), `--cpumask 15`
  (AVX2 without AVX-512) and the default AVX-512 lane on this host — all three
  lanes agree with each other and with the pre-change binary;
- a cropped 570x322 six-frame pair so that `vif_compute_line_residuals` runs a
  tail at every scale (285, 142, 71 columns are not multiples of 16);
- integer VIF `vif_enhn_gain_limit=1.0`, `vif_skip_scale0=true`, `debug=true`
  (every `write_scores` branch);
- float VIF `vif_prescale` with all four `vif_prescale_method` values
  (bicubic / lanczos4 / bilinear / nearest) and `vif_kernelscale=1.5` on both
  the scalar and the AVX2 lane; `speed_chroma` + `speed_temporal` for
  `vif_dec16_s` / `speed_get_antialias_filter`;
- float motion `motion_add_uv`, `motion_add_scale1`, `motion_filter_size=3`,
  `motion_filter_size=1`, `motion_force_zero`, `debug` + UV + scale1
  combined, UV on the scalar SAD lane, and the `motion_fps_weight` /
  `motion_blend_factor` / `motion_blend_offset` / `motion_max_val` clip paths.

### 4.3 Test suite

`meson test -C build --suite=fast --print-errorlogs` → `Ok: 105  Fail: 0`
(includes `test_vif_simd`, `test_motion_v2_simd`, `test_float_motion_coverage`,
`test_float_vif_coverage`, `test_vif_skip_scale0`, `test_integer_vif_log2`,
`test_motion_min_dim`, `test_float_vif_min_dim`). NEON (`test_vif_neon`,
`test_float_motion_neon`) needs an aarch64 runner and is covered by CI.

## 5. Alternatives considered (decision matrix)

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Cite-and-keep `NOLINT(readability-function-size)` on the nine oversized functions | Zero diff in the numeric code | ADR-0141 reserves NOLINT for load-bearing invariants; the sibling `_avx2` / `_avx512` kernels, not the scalar reference, are where an inline reduction is load-bearing; three copies of the statistic would keep drifting | Non-compliant with ADR-0141's "refactor first" |
| Macro-ise the shared statistic (`#define VIF_PIXEL_STAT(...)`) | Guarantees textual identity | Power-of-10 rule 8 (preprocessor restraint); `bugprone-macro-parentheses` noise; no type checking of the accumulator | Worse for lint than the problem it solves |
| `static FORCE_INLINE` helpers with verbatim expressions (chosen) | One scalar reference for three entry points and the SIMD tails; zero NOLINTs; provably bit-exact under the fork's compile flags | A future `-march`/`-ffp-contract=fast` change would need re-verification (the matrix in §4.2 is the recipe) | Only option satisfying ADR-0141 without new suppressions |
| Rewrite `NULL` → `nullptr` in the two C TUs that use it (`integer_vif.c`, `float_motion.c`) | No suppression bracket | MSVC `/std:clatest` does not document C `nullptr`; upstream-sync conflicts | Settled by ADR-1138 — file-scoped cited bracket in those two TUs; `vif_tools.c` has no null-pointer constants and carries none |
| Flatten `MotionState` back to `ref_u` / `blur_v[3]` fields and only split `init` / `extract` | Smaller struct diff | Every plane operation stays triplicated and each of the three functions still exceeds the branch threshold | Plane array is the smallest change that discharges all three NOLINTs |

## 6. Residue / follow-ups

- The Python golden gate (`make test-netflix-golden`) could not run on this
  host (`scipy` missing from the repo venv); the CLI measurements above use the
  exact argument form the Python runner builds, and CI runs the pytest gate.
- `constParameterCallback` on `flush()` is suppressed with a citation; a
  `const VmafFeatureExtractor *` callback type would be an ABI-adjacent change
  for a separate decision.
- The whole-program cppcheck `unusedFunction` rows on exported `vif_*` entry
  points are configuration-dependent (their callers live in other TUs) and are
  the same artefact the c-rework-core unit documented.

## References

- [ADR-0141](../adr/0141-touched-file-cleanup-rule.md) — touched-file lint rule.
- [ADR-0278](../adr/0278-t7-5-nolint-sweep.md) — NOLINT citation form.
- [ADR-0504](../adr/0504-float-convolution-avx512-port.md) — float VIF AVX2-only dispatch (comment preserved on `vif_use_avx2_convolution`).
- [ADR-0500](../adr/0500-vif-perf-lut-shrink-and-filter-cache.md) — VIF log2 LUT layout that `log_generate` fills.
- [ADR-1138](../adr/1138-c-translation-units-keep-null.md) — C translation units keep `NULL`; `integer_vif.c` and `float_motion.c` carry the file-scoped bracket, `vif_tools.c` has no null-pointer constants and carries none.
- clang-tidy debt baseline of 2026-08-31 (wave-1 dispatch).
