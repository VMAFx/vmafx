---
paths:
  - core/src/feature/hip/integer_adm_hip.c
  - core/src/feature/hip/integer_cambi_hip.c
  - core/src/feature/hip/integer_ssim_hip.c
invariant: HISS-21 unwind helpers must release allocated device resources in reverse allocation order.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# HISS-21 unwind helpers — release order is the invariant (2026-09-21)

`chore/hiss21-core-src-hip` replaced `goto`-based cleanup ladders in
`integer_adm_hip.c`, `integer_cambi_hip.c`, `integer_ms_ssim_hip.c`,
`integer_psnr_hvs_hip.c`, `speed_chroma_hip.c`, `speed_temporal_hip.c` and
`ssimulacra2_hip.c` with cascading `static` unwind helpers (HISS-01 / NASA
Rule 1), and split oversized init / submit / collect / close / score-writer
functions into cohesive `static` helpers (HISS-04 / NASA Rule 4).

`integer_adm_hip.c` is exception to helper NAMES below, not to
rules. #1507 rewrote that file's init and teardown while this branch was open:
its release path is straight-line cascade of paired acquire / release
helpers — `adm_hip_create_stream` / `adm_hip_destroy_stream`,
`adm_hip_load_modules` / `adm_hip_unload_modules`, `adm_hip_alloc_buffers` /
`adm_hip_free_buffers`, `adm_hip_alloc_luma` / `adm_hip_free_luma`,
`adm_hip_upload_buf` / `adm_hip_free_buf_dev` — driven by
`adm_hip_init_device()` and `init_fex_hip()`, with no `*_unwind_<label>()`
tier functions at all. `adm_hip_unwind_*` names this section used to
document no longer exist; do not resurrect them. Every rule below still binds
that file: same release set, same release order, real errno on every failure
exit, helpers `static` in same TU, and no arithmetic expression split
across helper boundary. other six files keep tier helpers as
described.

Rebase-sensitive invariants:

- **One helper per former label, tail-calling next-earlier tier.** Each
  `*_unwind_<label>()` reproduces exactly one former `fail_<label>:` body and
  then calls helper for label it used to fall into. release **set**
  and release **ORDER** on every exit path must stay byte-for-byte what
  ladder produced. Do not "simplify" cascade into single free-everything
  call: tier may deliberately skip earlier one (e.g. `speed_chroma`'s
  buffer-allocation failure leaves module loaded and stream alive,
  because those were claimed before buffers and are released by later
  failure, not this one).
- **tier's `hipError_t` argument must be real failure. Never
  `hipSuccess`.** Every ladder terminates in `hip_rc(rc)` / `ss2h_hip_rc(rc)`,
  which map `hipSuccess` to `0`. Handing tier `hipSuccess` therefore makes
  whole unwind return success: `init` reports 0,
  `vmaf_feature_extractor_context_init` sets `is_initialized`, and first
  `extract` or `submit` runs against buffers, modules, events and stream
  ladder has already released. Three call sites did exactly that until T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22:
  `integer_adm_hip.c`'s feature-name-dictionary failure (independently fixed
  by #1507, whose rewrite of that file is what tree now carries), and both
  allocator
  branches in `ssimulacra2_hip.c`; earlier revision of this file
  described behaviour as pre-existing and not to be fixed inside
  refactor. It was use-after-free. When failure is host allocation with
  no `hipError_t` of its own, capture ladder's result, and return
  caller's own errno (`-ENOMEM` for ADM dictionary;
  `ss2h_init_unwind_alloc()` forwards allocator's) unless ladder itself
  reports genuine HIP error.
- **Release from tier matching LAST successful allocation.**
  `integer_adm_hip.c`'s dictionary failure used to skip `d_dis_luma` and
  `d_ref_luma`; skip was inherited verbatim from pre-HISS-01
  `goto fail_host` whose label sat below `fail_ref_luma:`. Because
  `vmaf_feature_extractor_context_close` rejects uninitialised context,
  `close_fex_hip` never runs after failed `init`, so nothing downstream
  reclaims tier that is skipped — skipped tier is permanent leak, not
  deferral. `init_fex_hip()` now releases full set in exact reverse of
  acquisition order (`adm_hip_free_buf_dev`, `adm_hip_free_luma`,
  `adm_hip_free_buffers`, `adm_hip_unload_modules`, `adm_hip_destroy_stream`)
  and returns `-ENOMEM`. Any new resource acquired in `adm_hip_init_device()`
  gets its release added to BOTH that list and `close_fex_hip()`.
- **Helpers stay `static` and in same translation unit.** They exist so
  codegen stays equivalent to inline code they replace. Do not give them
  external linkage, do not route them through function pointers, and do not
  move them to shared header.
- **No arithmetic expression was split across helper boundary and no
  accumulation order changed.** `ms_ssim_hip_set_max_db()` and
  `sc_score_channel()` were lifted at statement boundaries precisely so
  scores stay bit-identical; `integer_adm_hip.c`'s score writers
  (`adm_hip_scale_scores()`, `adm_hip_append_scores()`) carry same
  constraint under #1507's names.
  HIP parity suite (`python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C <build> --suite hip`) before landing.
- **twins stay recognisable.** These files are deliberate twins of
  `../cuda/*.c`. unwind helpers mirror CUDA labels one-for-one and keep
  label names in helper names, so future CUDA-side port can be read
  against them.
- **`VmafOption` tables and `g_weights[108]` stay one entry per line, with
  no `clang-format` fence.** `options_hip[]` (`integer_adm_hip.c`),
  `options[]` (`integer_cambi_hip.c`), `options_chroma[]` /
  `options_temporal[]` (two SpEED HIP files) and `g_weights[108]`
  (`ssimulacra2_hip.c`) mirror their CPU and CUDA twins line for line, which
  is what makes three-way `diff` of ports readable. earlier revision
  of this branch hand-packed them behind `// clang-format off` to shrink
  HISS-04 "block" finding; praetor engine no longer counts file-scope
  initialiser table as function, so packing bought nothing and was
  reverted. Do not re-introduce it. Row content is load-bearing:
  `test_integer_adm_hip_option_table_mirrors_cpu` compares ADM table
  against CPU twin, so rebase must preserve every name, alias, help
  string, default, range, flag and array order.
- **three SSIMULACRA2 no-split citations are withdrawn — see
  [ADR-1289](../../../../../docs/adr/1289-hip-ssimulacra2-host-helper-split.md).**
  `ss2h_picture_to_linear_rgb()`, `ss2h_run_scale_gpu()` and
  `extract_fex_hip()` used to carry ADR-0141 §2 carve-out claiming that
  splitting them would break line-for-line diff against CPU source and
  CUDA twin. Praetor's touched-file rule has no such carve-out, and
  parity evidence is `core/test/test_hip_ssimulacra2_parity.c` plus
  ADR-0214 cross-backend gate, not diff, so three are split into
  `ss2h_yuv_primaries()`, `ss2h_upload_xyb()`, `ss2h_download_blurred()` and
  `ss2h_downsample_for_next_scale()` and `NOLINTNEXTLINE` lines are gone.
  What is still load-bearing: ADR-1205 / ADR-0891 `fmaf()` chain in
  per-pixel loop, eight-launch order inside `ss2h_run_scale_gpu()`, and
  per-scale order in `extract_fex_hip()`. `../cuda/ssimulacra2_cuda.c` and
  `core/src/feature/ssimulacra2.c` are still un-split and keep their own
  citations; when CUDA HISS-21 slice lands it should mirror these four
  helper names and boundaries so twins read against each other again.
  A split that leaves no-split citation in place, or moves one onto
  extracted helper, is still defect — that is what ADR-1289 fixes here.
