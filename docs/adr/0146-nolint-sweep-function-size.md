# ADR-0146: Sweep `readability-function-size` NOLINTs from libvmaf

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: lint, cleanup, refactor, touched-file-rule

## Context

Prior to this PR, `core/src/` carried 20 `// NOLINTNEXTLINE(readability-function-size)`
suppressions covering functions that exceeded the `.clang-tidy` budget
(LineThreshold=60, StatementThreshold=120, BranchThreshold=15,
NestingThreshold=4). Seventeen of those sat in fork-touched infrastructure
files (dict, picture, picture pool, predict, read_json_model, feature
extractor, feature collector, libvmaf top-level, output) carrying the bulk
of the public-API surface; three sat in IQA / SIMD code
(`_iqa_convolve`, `_iqa_ssim`, `vif_statistic_s_avx2`).

[ADR-0141](0141-touched-file-cleanup-rule.md) ("touched-file lint-clean")
earmarked these as pre-rule historical debt and queued a single sweep-PR
(`T7-5` in [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md)).
Every existing NOLINT here carried a justification comment of the form
"Refactor deferred to backlog item T7-5 …". This PR is that sweep.

## Decision

Refactor every `readability-function-size` NOLINT site so each function
sits below the `.clang-tidy` budget without a suppression. No NOLINT stays
behind for this check after this PR.

**Fork-local / fork-touched files** — small `static` helpers extracted
above each caller in the same TU; behaviour preserved:

| File | Refactored function(s) | Extracted helpers |
| --- | --- | --- |
| `dict.c` | `vmaf_dictionary_set` | `dict_ensure_allocated`, `dict_normalize_numeric`, `dict_grow_entries`, `dict_overwrite_existing`, `dict_append_new_entry` |
| `picture.c` | `vmaf_picture_alloc` | `picture_compute_geometry`, `picture_acquire_buffer` |
| `picture_pool.c` | `vmaf_picture_pool_init` | `pool_preallocate_pictures` |
| `predict.c` | `piecewise_linear_mapping`, `vmaf_predict_score_at_index`, `vmaf_bootstrap_predict_score_at_index` | `piecewise_segment_apply`, `predict_resolve_feature_name`, `predict_ensure_caches`, `predict_load_feature_score`, `predict_build_svm_nodes`, `bootstrap_gather_scores`, `bootstrap_compute_statistics`, `bootstrap_transform_and_clip`, `bootstrap_append_named_scores` |
| `feature/feature_extractor.c` | `vmaf_fex_ctx_pool_aquire` | `ctx_pool_ensure_slot_ctx`, `ctx_pool_claim_slot` |
| `feature/feature_collector.c` | `vmaf_feature_collector_append` | `feature_collector_grow_capacity`, `feature_collector_ensure_vector`, `feature_collector_run_model_predict`, `feature_collector_dispatch_metadata` |
| `libvmaf.c` | `threaded_read_pictures`, `flush_context`, `vmaf_read_pictures` | `threaded_enqueue_one`, `flush_context_serial`, `flush_context_cuda`, `flush_context_sycl`, `read_pictures_cuda_translate`, `read_pictures_cuda_cleanup`, `read_pictures_sycl_prep`, `read_pictures_should_skip`, `read_pictures_dispatch_one`, `read_pictures_validate_and_prep`, `read_pictures_extractor_loop`, `read_pictures_update_prev_ref`, `read_pictures_post_extractor` |
| `output.c` | `vmaf_write_output_xml`, `vmaf_write_output_json` | `count_written_at`, `xml_write_frames`, `xml_write_pooled_and_aggregate`, `json_write_frame_metric`, `json_write_frame`, `json_write_frames`, `json_write_pool_score`, `json_write_pooled_entry`, `json_write_pooled`, `json_write_aggregate` |
| `read_json_model.c` | `parse_feature_opts_dicts`, `parse_score_transform`, `parse_model_dict`, `model_collection_parse` | `parse_feature_opts_entry`, `parse_feature_opts_object`, `parse_score_transform_poly`, `parse_score_transform_knots_key`, `parse_score_transform_bool_str`, `parse_score_transform_entry`, `parse_model_dict_score_transform`, `parse_model_dict_model_type`, `parse_model_dict_norm_type`, `parse_model_dict_score_clip`, `parse_model_dict_array_key`, `parse_model_dict_entry`, `model_collection_read_one`, `model_collection_parse_loop` |

**IQA / SIMD files** — extracted `static inline` helpers that preserve the
ADR-0138 / ADR-0139 bit-exactness contracts:

- `feature/iqa/convolve.c` — `_iqa_convolve` split into
  `iqa_convolve_horizontal_pass` + `iqa_convolve_vertical_pass` +
  `iqa_convolve_1d_separable` (for `IQA_CONVOLVE_1D`) and
  `iqa_convolve_2d`. Driver becomes a thin `#ifdef` dispatcher.
  Also renamed the TU-local `_calc_scale` to `iqa_calc_scale`
  (static-local, reserved-identifier cleanup) and tidied the
  out-of-band `_iqa_img_filter` / `_iqa_filter_pixel` declarations to
  stay compliant with `readability-isolate-declaration` /
  `bugprone-implicit-widening-of-multiplication-result`.
- `feature/iqa/ssim_tools.c` — `_iqa_ssim` split into
  `ssim_workspace_alloc`, `ssim_workspace_free`, `ssim_compute_stats`,
  `ssim_init_args` wrapped by a `struct ssim_workspace`. The
  per-SIMD-dispatch convolve call ordering is preserved verbatim so
  bit-exactness is unchanged.
- `feature/x86/vif_statistic_avx2.c` — `vif_statistic_s_avx2` split
  into `vif_stat_simd8_compute` (load + clamp + ratio stage) +
  `vif_stat_simd8_reduce` (log2 + mask + per-lane scalar-float
  reduction) + `vif_stat_scalar_pixel` + `vif_stat_simd8_block` (thin
  composer) + `vif_stat_consts_init`. The ADR-0139 per-lane
  scalar-float reduction via the 32-byte aligned `tmp_n[8]` / `tmp_d[8]`
  arrays is preserved exactly; both lane-state handoffs go through an
  explicit `struct vif_simd8_lane` so the compiler still inlines the
  `__m256` registers without spilling.

**Bonus cleanup** — while touching the files above, a few previously-existing
minor warnings surfaced (pre-existing `readability-braces-around-statements`
in `score_compare`, pre-existing `bugprone-implicit-widening-of-multiplication-result`
on `calloc(w * h, ...)` inside the convolve cache allocation, pre-existing
`readability-non-const-parameter` on `model_collection_parse_loop`'s
`cfg_name` — fixed by writing through the original pointer rather than the
aliased `c->name`). The ADR-0141 touched-file rule requires fixing these;
this PR does.

**NOLINTs kept** (justified in-place, not the target of this sweep):

- `picture.c` — one `// NOLINTNEXTLINE(performance-no-int-to-ptr)` on
  the tagged-value cast in the picture pool release-callback
  registration (intentional `(void *)(uintptr_t)` idiom; never
  dereferenced).
- `output.c` — `// NOLINTBEGIN(cert-err33-c) … NOLINTEND` wrapping the
  large `fprintf`-heavy XML/JSON writers where every `fprintf` return
  value discard would force a per-call `(void)` cast with no caller
  benefit (output already handles I/O failure via `ferror` checks).
- `predict.c` — scoped `NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)`
  around a deliberate extra-guard cast.
- `read_json_model.c` — scoped `NOLINTBEGIN(clang-analyzer-unix.Malloc)`
  in `model_collection_read_one` where ownership of `m` is transferred
  cross-parameter (the analyzer can't follow the handoff).
- `libvmaf.c`, `mem.c` — `bugprone-reserved-identifier` on the
  libvmaf-public `_vmaf_*` symbols is upstream-API convention.
- `svm.cpp`, `pdjson.c` — whole-file `NOLINTBEGIN…NOLINTEND`;
  upstream-verbatim code we forked from libsvm / pdjson.
- `dnn/ort_backend.c`, `dnn/dnn_api.c`, `dnn/model_loader.c` — small
  scoped NOLINTs for ONNX Runtime C API parameter-const idiosyncrasies
  and for a deliberate buffer-end NUL-byte write.

None of these suppressions cover `readability-function-size`. After this
PR, a `grep -rn 'NOLINT.*readability-function-size' core/src/` returns
zero matches.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Keep the three IQA / SIMD NOLINTs with updated justification** | Upstream parity for `_iqa_*` helps rebase; AVX2 split risks bit-exactness drift | Skips the core of T7-5; leaves "deferred to T7-5" language in the tree referencing itself; regresses the principle from `feedback_no_lint_skip_upstream.md` | Rejected after popup 2026-04-24 "Refactor all three". Bit-exactness verified under VMAF_CPU_MASK=0/255 diff (exit 0) |
| **Split the sweep into 3-4 follow-up PRs (one per file cluster)** | Smaller reviews; easier to bisect regressions | Defeats the "one sweep-PR" scoping that ADR-0141 §Historical debt targeted; each PR triggers its own full gate matrix | Rejected — the sweep is mechanical; review cost scales with the touch set, not with N PRs |
| **Raise `.clang-tidy` thresholds (LineThreshold=90, StatementThreshold=180)** | Zero refactoring work | Dilutes the NASA/JPL Power of 10 §4 rule the fork subscribes to; hides the next round of oversized functions | Rejected — the threshold is the contract; functions that exceed it really do benefit from splitting |
| **Delegate the entire sweep to a single agent, no human review** | Faster | Every refactor needs a bit-exactness check + clang-tidy pass; load-bearing SIMD invariants require per-file judgment | Rejected — the fork-local (safe) files were delegated to a general-purpose agent and reviewed; the three bit-exact files (iqa_convolve / iqa_ssim / vif_avx2) were done by hand with per-file verification |

## Consequences

- **Positive**:
  - `core/src/` has zero `readability-function-size` NOLINTs; the
    ADR-0141 touched-file rule no longer carries historical debt for
    this check.
  - Public-API entry points (`vmaf_read_pictures`, `flush_context`,
    `vmaf_predict_score_at_index`, `vmaf_picture_alloc`,
    `vmaf_dictionary_set`, `vmaf_feature_collector_append`,
    `vmaf_write_output_xml/json`, model-JSON readers) are now
    composed of named helpers — easier to unit-test, easier to read
    in stack traces, easier to reason about during CPU / CUDA / SYCL
    review.
  - Ad-hoc lint cleanups landed alongside: the `calloc(w*h, ...)`
    widening, the multi-declaration `score_compare` / `_iqa_filter_pixel`
    forms, the `model_collection_parse_loop` alias-write, the
    `_calc_scale` → `iqa_calc_scale` rename. Zero new NOLINTs
    introduced.
- **Negative**:
  - Upstream parity cost on `_iqa_convolve` / `_iqa_ssim`: a future
    rebase that touches these upstream functions will conflict against
    the fork's split shape. Mitigated by
    [`docs/rebase-notes.md`](../rebase-notes.md) entry 0039 and an
    `AGENTS.md` rebase-sensitive invariant; the helper names are
    predictable (`iqa_convolve_1d_separable`, `ssim_compute_stats`,
    etc.) so a future rebase can recompose them inline if needed.
  - AVX2 `vif_statistic_s_avx2` now has four helpers rather than one
    120-line function; the bit-exactness contract (ADR-0139 per-lane
    scalar-float reduction) is now a post-condition distributed
    across `vif_stat_simd8_reduce` rather than inlined. Mitigated by
    the rebase-notes entry citing both ADRs.
- **Neutral / follow-ups**:
  - `_iqa_*` reserved-identifier NOLINTs remain elsewhere in the tree
    (upstream-mirrored callers in `ssim.c`, `ms_ssim.c`, `float_ms_ssim.c`).
    Those files are out of scope here; follow-up T7-6 to decide
    whether to rename the IQA API surface in a separate PR.
  - ADR-0141 §Historical debt can delete its reference to T7-5 once
    this PR lands.

## Verification

- `meson test -C build` → 32/32 pass.
- `VMAF_CPU_MASK=0` vs `VMAF_CPU_MASK=255` full-pipeline run on
  `src01_hrc00/01_576x324` (Netflix golden-pair #1) — **VMAF score,
  VIF, ADM, MOTION, and SSIM all bit-identical** (`diff` exit 0 after
  stripping the `fps` line).
- `VMAF_CPU_MASK=0/255` with `--feature float_ssim --feature float_ms_ssim`
  on the same pair — bit-identical.
- `clang-tidy -p build` on all 12 touched C files — zero warnings
  (no `readability-function-size`, no `readability-isolate-declaration`,
  no `bugprone-reserved-identifier`, no `performance-no-int-to-ptr`
  leaked past the preserved justified NOLINT).

## References

- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — IQA convolve
  widen-then-add pattern (must be preserved on any refactor).
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — SSIM per-lane
  scalar-double reduction pattern (preserved on vif_statistic_s_avx2
  refactor by threading the `__m256` state through
  `struct vif_simd8_lane`).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule; §Historical debt scoped T7-5.
- Backlog: `.workingdir2/BACKLOG.md` T7-5.
- User direction 2026-04-24 popup: "Refactor all three"
  (for `_iqa_convolve`, `_iqa_ssim`, `vif_statistic_s_avx2`).
