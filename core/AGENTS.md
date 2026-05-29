# AGENTS.md — libvmaf

Orientation for any coding agent working inside `libvmaf/`. Root orientation
lives in [../AGENTS.md](../AGENTS.md); this file is the scoped hand-off for
this subtree. Claude Code equivalents in [../CLAUDE.md](../CLAUDE.md).

## Scope

The C engine — VMAF metric, feature extractors, backends, public API,
CLI (`tools/vmaf`, `tools/vmaf_bench`), and C unit tests.

```
libvmaf/
  include/libvmaf/   # public C API (libvmaf.h, dnn.h, model.h, picture.h, ...)
  src/               # engine + feature extractors + backends
    cuda/            # CUDA backend runtime (picture, dispatch, ring buffer)
    sycl/            # SYCL backend runtime (queue, USM, dmabuf import)
    dnn/             # ONNX Runtime integration (tiny AI)
    feature/         # per-feature CPU implementations
      x86/           # AVX2 / AVX-512 SIMD paths
      arm64/         # NEON SIMD paths
      cuda/          # CUDA feature kernels
      sycl/          # SYCL feature kernels
  test/              # C unit tests (µnit-style: test.h + mu_run_test)
  tools/             # vmaf CLI, vmaf_bench, cli_parse
  meson.build
  meson_options.txt
```

## Ground rules for this subtree

- **Coding standards**: NASA/JPL Power of 10 + JPL-C-STD + SEI CERT C (see
  [../docs/principles.md](../docs/principles.md)). `.clang-tidy` enforces.
- **License headers**: Netflix-header-preserving for upstream-touched files;
  `Copyright 2026 Lusoris and Claude (Anthropic)` for wholly-new files.
  See [ADR-0025](../docs/adr/0025-copyright-handling-dual-notice.md).
- **Style**: K&R, 4-space, 100-char columns, `.clang-format` authoritative.
- **Banned functions** (see `docs/principles.md §1.2 rule 30`): `gets`,
  `strcpy`, `strcat`, `sprintf`, `strtok`, `atoi`, `atof`, `rand`, `system`.
- **Every non-void return value is checked or explicitly `(void)`-discarded.**
- **Every new file starts with the license header** (Netflix preserved on
  upstream-touched; Lusoris/Claude on wholly-new — see ADR-0025).

## Workflows routed here

| Task | Skill |
| --- | --- |
| Add a feature extractor | [../.claude/skills/add-feature-extractor/SKILL.md](../.claude/skills/add-feature-extractor/SKILL.md) |
| Add a SIMD path (AVX2 / AVX-512 / NEON) | [../.claude/skills/add-simd-path/SKILL.md](../.claude/skills/add-simd-path/SKILL.md) |
| Add a GPU backend (CUDA / SYCL / HIP / Vulkan) | [../.claude/skills/add-gpu-backend/SKILL.md](../.claude/skills/add-gpu-backend/SKILL.md) |
| Register a model JSON | [../.claude/skills/add-model/SKILL.md](../.claude/skills/add-model/SKILL.md) |
| Cross-backend numeric diff | [../.claude/skills/cross-backend-diff/SKILL.md](../.claude/skills/cross-backend-diff/SKILL.md) |
| Profile a hot path | [../.claude/skills/profile-hotpath/SKILL.md](../.claude/skills/profile-hotpath/SKILL.md) |

## Governing ADRs

- [ADR-0119](../docs/adr/0119-cli-precision-default-revert.md) — CLI precision default `%.6f` (Netflix-compat golden gate); `--precision=max` opts in to `%.17g`. Propagates to `output.c` and Python. Supersedes [ADR-0006](../docs/adr/0006-cli-precision-17g-default.md).
- [ADR-0012](../docs/adr/0012-coding-standards-jpl-cert-misra.md) — the coding-standards stack.
- [ADR-0022](../docs/adr/0022-inference-runtime-onnx.md) — execution-provider mapping ORT↔backends.
- [ADR-0024](../docs/adr/0024-netflix-golden-preserved.md) — golden-data gate (three CPU reference pairs, never modified).
- [ADR-0025](../docs/adr/0025-copyright-handling-dual-notice.md) — dual-copyright policy.
- [ADR-0137](../docs/adr/0137-thread-local-locale-for-numeric-io.md) —
  thread-local locale abstraction (`thread_locale.h`) for all numeric I/O.

## Rebase-sensitive invariants

- **`feature_extractor_vector_append()` deduplicates by provided-feature
  names, not extractor name** (fork-local, ADR-0385 / T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE):
  [`src/fex_ctx_vector.c`](src/fex_ctx_vector.c) uses
  `provided_features_overlap()` to detect CPU/GPU twins before
  registering a new extractor. The old dedup key was derived from
  `vmaf_feature_name_from_options(fex->name, …)`, which produced
  `"adm"` vs `"adm_cuda"` — two distinct strings — so both twins were
  registered and both wrote the same collector slot. Any upstream sync
  that rewrites `fex_ctx_vector.c` must preserve the provided-feature
  dedup path; reverting to name-only dedup re-opens the double-write
  regression on every GPU-enabled binary when `--feature <name>` is
  combined with a default model load.
- **`picture_compute_geometry` stride alignment uses `unsigned` + `1u`
  mask** (fork-local, round-5 `-fsanitize=integer` sweep):
  `aligned_y` and `aligned_c` in
  [`src/picture.c`](src/picture.c) are declared `const unsigned` and
  the bitmask uses `DATA_ALIGN - 1u` (not `DATA_ALIGN - 1`) so
  the complement stays in unsigned domain and avoids a signed→unsigned
  implicit conversion that fires with `-fsanitize=integer`. If an
  upstream sync rewrites `picture_compute_geometry`, preserve the
  `unsigned` type and `1u` literal. See
  [docs/rebase-notes.md](../docs/rebase-notes.md)
  §PR-fix-picture-align-unsigned-narrowing.
- **`vmaf_init` cpumask narrowing uses an explicit `(unsigned)` cast**
  (fork-local, round-5 `-fsanitize=integer` sweep):
  `vmaf_set_cpu_flags_mask((unsigned)(~cfg.cpumask))` in
  [`src/libvmaf.c`](src/libvmaf.c). The cast is deliberate: all
  defined CPU flag bits fit in 6 bits; the high 32 bits of the
  `uint64_t cpumask` complement are always zero for any valid input.
  Do not remove the explicit cast.
- **Output writers return `ferror(outfile) ? -EIO : 0`.**
  `vmaf_write_output_{xml,json,csv,sub}` in
  [src/output.c](src/output.c) use a single tail `return` that
  checks `ferror(outfile)` — per [ADR-0119](../docs/adr/0119-cli-precision-default-revert.md).
  Any upstream patch that changes the tail to bare `return 0`
  must be merged so the fork's `ferror` check survives. The
  thread-locale bracket from [ADR-0137](../docs/adr/0137-thread-local-locale-for-numeric-io.md)
  is `push_c()` at entry → body → `pop()` before the `ferror`
  check; dropping the `pop()` leaks a `locale_t` on POSIX and
  leaves the calling thread locked to `"C"` on Windows.
- **JSON model loader has no fixed feature/knot schema ceiling.**
  [`src/read_json_model.c`](src/read_json_model.c) grows
  `VmafModel.feature` and `score_transform.knots.list` from the JSON
  payload. Do not restore the old `MAX_FEATURE_COUNT` / `MAX_KNOT_COUNT`
  rejection pattern during an upstream sync; external model JSONs with
  65+ features or 11+ piecewise-linear knots must parse if the payload
  is otherwise valid. The regression coverage lives in
  [`test/test_model.c`](test/test_model.c).
- **HIP backend scaffold contract** (fork-local, ADR-0212 / T7-10):
  the `enable_hip=true` build path compiles
  [src/hip/](src/hip/) and [src/feature/hip/](src/feature/hip/)
  into `libvmaf_feature_static_lib` and exposes the public C-API
  entry points in
  [include/libvmaf/libvmaf_hip.h](include/libvmaf/libvmaf_hip.h)
  (`vmaf_hip_state_init` / `_import_state` / `_state_free` /
  `vmaf_hip_list_devices` / `vmaf_hip_available`). Until the
  runtime PR (T7-10b) lands, every public entry point returns
  `-ENOSYS` and the smoke test
  [test/test_hip_smoke.c](test/test_hip_smoke.c) pins that
  contract. Any rebase or refactor that "succeeds" the scaffold
  (e.g. accidentally enables a code path) without flipping the
  smoke expectations breaks the rebase story for the runtime PR.
  The `dependency('hip-lang')` probe in
  [src/hip/meson.build](src/hip/meson.build) stays
  `required: false` for the scaffold; flipping to `true` belongs
  to the runtime PR. The `enable_hip` option type is
  `boolean` (matching `enable_cuda` / `enable_sycl`); do NOT
  convert it to `feature` without an ADR amendment per ADR-0212
  § "Decision".
- **Thread-pool job recycling + inline data buffer** (fork-local,
  ADR-0147): [`src/thread_pool.c`](src/thread_pool.c) recycles
  `VmafThreadPoolJob` slots via a `pool->free_jobs` free list
  (mutex-protected by `queue.lock`) and stores payloads ≤
  `JOB_INLINE_DATA_SIZE` (64 bytes) inside `job->inline_data`
  instead of a second `malloc`. The cleanup path distinguishes
  inline from heap payloads via the
  `job->data != job->inline_data` guard in
  `vmaf_thread_pool_job_clear_data`; do not collapse this
  check during a rebase — freeing `inline_data` would corrupt
  the slot. The fork's `func(void *data, void **thread_data)`
  signature and `VmafThreadPoolWorker` per-worker-data path must
  survive any upstream sync; Netflix upstream PR #1464 (closed)
  has a similar job-pool but uses the bare
  `func(void *data)` signature — on conflict keep the fork's
  two-arg signature and merge only the pool-mechanics changes.
  The struct carries an immutable `n_workers_created` field (written
  once in `pool_create`, never decremented) alongside the live
  `n_threads` counter (decremented by each exiting runner thread under
  `queue.lock`). `destroy` reads `n_workers_created` — not `n_threads`
  — to iterate `workers[]` for `thread_data_free`; do not collapse
  these two counters back into one during a rebase or the `destroy`
  path reacquires a data race (C11 UB, TSan-detected). See
  [Research-0097](../docs/research/0097-thread-pool-pthread-create-unchecked-2026-05-10.md).
  See [ADR-0147](../docs/adr/0147-thread-pool-job-pool.md) and
  [rebase-notes 0040](../docs/rebase-notes.md).

- **`integer_vif` is luma-only across every backend** (fork-local,
  [ADR-0541](../docs/adr/0541-integer-vif-luma-only-clarification.md)).
  CPU [`src/feature/integer_vif.c`](src/feature/integer_vif.c) reads
  `data[0]` only and has no `enable_chroma` option; CUDA
  [`src/feature/cuda/integer_vif_cuda.c`](src/feature/cuda/integer_vif_cuda.c)
  hardcodes `s->n_planes = 1` and warn-on-trues `enable_chroma`; HIP,
  SYCL, Vulkan, Metal twins all match. Upstream Netflix/vmaf is the
  same. VIF (Sheikh & Bovik, 2006) is defined on a single luminance
  channel — multi-plane VIF has no MOS-correlation literature. Do not
  "fix" the `n_planes = 1` or "wire enable_chroma through" without
  filing a fresh ADR that includes a research digest and a golden-data
  regeneration plan; the previous attempt (PRs #948 + #949, 2026-05-16)
  was abandoned and left the vestigial CUDA `enable_chroma` option as
  the only artefact. The regression test
  [`test/test_integer_vif_cpu_cuda_parity.c`](test/test_integer_vif_cpu_cuda_parity.c)
  asserts CPU vs CUDA scale parity and `enable_chroma=true` bit-identity
  with the default invocation — both must keep passing.

- **Vulkan PSNR chroma contract** (fork-local, [ADR-0216](../docs/adr/0216-vulkan-chroma-psnr.md)).
  [`src/feature/vulkan/psnr_vulkan.c`](src/feature/vulkan/psnr_vulkan.c)
  carries `ref_in[3] / dis_in[3] / se_partials[3]` arrays in
  `PsnrVulkanState` (Y / Cb / Cr) and dispatches the same
  `psnr.comp` shader once per active plane in a single command
  buffer. The shader is plane-agnostic — it reads
  `(width, height, num_workgroups_x)` from push constants — so
  rebases that "simplify" the chroma loop back to a single luma
  dispatch will silently regress `psnr_cb` / `psnr_cr` to CPU
  fall-through (and break the `cross_backend_vif_diff.py
  --feature psnr` gate, which now asserts on Y / Cb / Cr). YUV400
  is the only supported `n_planes = 1` path; the `pix_fmt`
  branch in `init` mirrors the `enable_chroma = false` clamp in
  CPU `integer_psnr.c::init` and must follow it on any future
  `min_sse` / `psnr_max[p]` divergence. The descriptor pool is
  sized for 12 sets (4 frames in flight × 3 planes) — do not
  shrink without re-checking lavapipe behaviour under
  frames-in-flight > 1.

- **Embedded MCP runtime contract** (fork-local, [ADR-0209](../docs/adr/0209-mcp-embedded-scaffold.md)).
  [`src/mcp/`](src/mcp/) now contains the promoted in-process MCP
  runtime declared in
  [`include/libvmaf/libvmaf_mcp.h`](include/libvmaf/libvmaf_mcp.h):
  stdio, UDS, and loopback-SSE transports, plus read-only
  `list_features` and out-of-band `compute_vmaf`. Preserve the
  early argument validation (`-EINVAL` on NULLs / negative fds /
  NULL paths) before any runtime work; the smoke tests for `_init`,
  `_start_uds`, `_start_stdio`, and `_start_sse` rely on that
  contract. `compute_vmaf` must keep using a per-call ephemeral
  `VmafContext`, not the host scorer, because pooled scoring commits
  models destructively. The `enable_mcp` umbrella flag must default
  `false` until mutating measurement-thread tools and the SPSC bridge
  land; the silent-flip risk is the same as ADR-0175's Vulkan
  precedent.

- **MS-SSIM `enable_lcs` GPU contract** (fork-local,
  [ADR-0243](../docs/adr/0243-enable-lcs-gpu.md)).
  [`src/feature/cuda/integer_ms_ssim_cuda.c`](src/feature/cuda/integer_ms_ssim_cuda.c)
  and
  [`src/feature/vulkan/ms_ssim_vulkan.c`](src/feature/vulkan/ms_ssim_vulkan.c)
  emit 15 extra metrics — `float_ms_ssim_{l,c,s}_scale{0..4}` —
  when the `enable_lcs` option is true, mirroring the CPU
  `float_ms_ssim` extractor in
  [`src/feature/float_ms_ssim.c`](src/feature/float_ms_ssim.c#L189-L221).
  The metric names, ordering (metric-wise — all `l_scale*` first,
  then `c_*`, then `s_*`), and `places=4` cross-backend contract
  are part of the public API surface; do not rename, reorder, or
  introduce per-backend variations. The kernels themselves
  (`ms_ssim_vert_lcs` CUDA / vert pass in `ms_ssim.comp` Vulkan)
  already compute the per-scale `l_means[i]` / `c_means[i]` /
  `s_means[i]` doubles — gating only the host-side
  `vmaf_feature_collector_append` calls keeps default-path
  (`enable_lcs=false`) output bit-identical to the pre-T7-35
  binary. The cross-backend gate's `float_ms_ssim_lcs`
  pseudo-feature in
  [`scripts/ci/cross_backend_vif_diff.py`](../scripts/ci/cross_backend_vif_diff.py)
  and
  [`scripts/ci/cross_backend_parity_gate.py`](../scripts/ci/cross_backend_parity_gate.py)
  enforces the contract; do not drop the `FEATURE_ALIASES` entry
  or the matching `FEATURE_TOLERANCE` row on a rebase.

- **GPU-parity matrix gate contract** (fork-local,
  [ADR-0214](../docs/adr/0214-gpu-parity-ci-gate.md)).
  [`scripts/ci/cross_backend_parity_gate.py`](../scripts/ci/cross_backend_parity_gate.py)
  is the single source of truth for the per-feature absolute
  tolerance every (CPU↔GPU, GPU↔GPU) cell must respect. The CI
  job `vulkan-parity-matrix-gate` in
  [tests-and-quality-gates.yml](../.github/workflows/tests-and-quality-gates.yml)
  runs it on every PR over CPU↔Vulkan/lavapipe; CUDA/SYCL/hardware-
  Vulkan are advisory until a self-hosted runner exists. Do not
  tighten a `FEATURE_TOLERANCE` entry without a measurement-driven
  follow-up ADR (per CLAUDE.md §12 r1). Adding a new feature with
  a GPU twin requires (1) a `FEATURE_METRICS` entry, (2) a
  `FEATURE_TOLERANCE` entry if the feature relaxes places=4, and
  (3) a row in
  [`docs/development/cross-backend-gate.md`](../docs/development/cross-backend-gate.md).

- **`float_motion` extra-options surface (upstream port from Netflix
  b949cebf, 2026-04-29).** [`src/feature/float_motion.c`](src/feature/float_motion.c)
  exposes four extra options (`motion_add_scale1`, `motion_add_uv`,
  `motion_filter_size`, `motion_max_val`) and emits a `motion3_score` on
  the second frame. The default Y-plane / scale-0 path stays bit-identical
  to the pre-port baseline by routing through `compute_motion_simd()` (the
  AVX2 / AVX-512 / NEON `float_sad_line` dispatch); the non-default paths
  (`scale1`, UV) fall through to scalar `compute_motion()` in
  [`src/feature/motion.c`](src/feature/motion.c). The
  `picture_copy()` / `picture_copy_hbd()` signature in
  [`src/feature/picture_copy.{c,h}`](src/feature/picture_copy.h) gained a
  trailing `int channel` parameter (upstream d3647c73 prerequisite); every
  fork-local caller (`float_adm.c`, `float_ansnr.c`, `float_moment.c`,
  `float_ms_ssim.c`, `float_psnr.c`, `float_ssim.c`, `float_vif.c`,
  `cuda/integer_ms_ssim_cuda.c`, `sycl/integer_ms_ssim_sycl.cpp`,
  `sycl/integer_ssim_sycl.cpp`, `vulkan/ms_ssim_vulkan.c`,
  `vulkan/ssim_vulkan.c`) passes `0` for the Y-plane. On future upstream
  syncs, do not drop the SIMD fast-path wrapper: the NASA/JPL Power-of-10
  inner-loop budget still demands it, and the Netflix golden-data gate
  ([ADR-0024](../docs/adr/0024-netflix-golden-preserved.md)) is regression-
  flagging if the default path stops dispatching to `vmaf_image_sad_avx2`
  / `_avx512` / `_neon`. See
  [`docs/rebase-notes.md` §0049](../docs/rebase-notes.md).

- **icpx-aware clang-tidy wrapper for SYCL TUs** (fork-local,
  [ADR-0217](../docs/adr/0217-sycl-toolchain-cleanup.md)).
  [`scripts/ci/clang-tidy-sycl.sh`](../scripts/ci/clang-tidy-sycl.sh)
  is the single entry point for linting `libvmaf/src/sycl/**` and
  `libvmaf/src/feature/sycl/**` files; it injects the oneAPI SYCL
  include path + `-D__SYCL_DEVICE_ONLY__=0` so stock LLVM clang-tidy
  resolves `<sycl/sycl.hpp>`. The CI lane
  `Clang-Tidy SYCL (Changed Files, Advisory)` in
  [`.github/workflows/lint-and-format.yml`](../.github/workflows/lint-and-format.yml)
  runs the wrapper over a SYCL build tree; do not invoke stock
  `clang-tidy` directly against SYCL TUs (will surface
  `'sycl/sycl.hpp' file not found` clang-diagnostic-errors). When
  adding a new SYCL TU, no AGENTS.md update is needed — the wrapper
  finds it via the changed-file diff. The wrapper resolves the icpx
  install via `$ICPX_ROOT` (override) or
  `/opt/intel/oneapi/compiler/latest` (default); if Intel
  reorganises this layout in a future release the wrapper's candidate
  list needs the new path added (see the `for cand in ...` block in
  the script). Companion bench-time helper:
  [`scripts/ci/sycl-bench-env.sh`](../scripts/ci/sycl-bench-env.sh).
- **GPU long-tail terminus reached** (fork-local, T7-36 closure
  via [ADR-0210](../docs/adr/0210-cambi-vulkan-integration.md)).
  Every registered feature extractor now has at least one GPU twin
  — cambi was the last remaining gap. lpips remains ORT-delegated
  per [ADR-0022](../docs/adr/0022-inference-runtime-onnx.md).
  Adding a new feature extractor without a same-PR GPU twin is now
  an explicit choice — record the deferral in the ADR body.
  Governing batches:
  [ADR-0182](../docs/adr/0182-gpu-long-tail-batch-1.md) (1) +
  [ADR-0188](../docs/adr/0188-gpu-long-tail-batch-2.md) (2) +
  [ADR-0192](../docs/adr/0192-gpu-long-tail-batch-3.md) (3).
- **`motion3_score` GPU contract (T3-15(c) / ADR-0219).** The three GPU
  motion twins (`src/feature/vulkan/motion_vulkan.c`,
  `src/feature/cuda/integer_motion_cuda.c`,
  `src/feature/sycl/integer_motion_sycl.cpp`) emit
  `VMAF_integer_feature_motion3_score` in 3-frame window mode by
  applying CPU's host-side post-process to motion2: `clip(motion_blend(
  motion2 * motion_fps_weight, motion_blend_factor,
  motion_blend_offset), motion_max_val)` with optional moving-average.
  No device-side state is added — motion3 is a deterministic scalar
  function of motion2. Two invariants the rebase story depends on:
  (1) `motion_five_frame_window=true` returns `-ENOTSUP` at `init()`
  (the 5-deep blur ring + second SAD pair are still deferred — do
  not silently fall back to the 3-frame path); (2) any Netflix
  upstream sync that touches `motion_blend()` in
  [`motion_blend_tools.h`](src/feature/motion_blend_tools.h), the
  `motion_max_val` clip, or the moving-average rule MUST mirror the
  change into `motion3_postprocess_*` across all three GPU files
  in the same PR. The cross-backend parity gate at `places=4`
  (`scripts/ci/cross_backend_parity_gate.py` +
  `scripts/ci/cross_backend_vif_diff.py` `FEATURE_METRICS["motion"]`
  → `integer_motion3`) catches drift, but only after a full GPU
  run. See [`docs/rebase-notes.md` §0219](../docs/rebase-notes.md).

- **Symbol visibility: every new public entry point needs `VMAF_EXPORT`**
  (fork-local, [ADR-0379](../docs/adr/0379-libvmaf-symbol-visibility.md) /
  Research-0092). `libvmaf/src/meson.build` compiles all TUs with
  `-fvisibility=hidden`; only symbols annotated with `VMAF_EXPORT`
  (defined in `libvmaf/include/libvmaf/macros.h`) appear in the
  dynamic symbol table of `libvmaf.so`. When adding a new public C
  entry point, apply `VMAF_EXPORT` to its declaration in the installed
  public header — the attribute propagates from declaration to
  definition if the definition TU includes the header, so no annotation
  of the definition itself is normally required. Exception: if the
  definition TU does *not* include the public header (see
  `src/dnn/model_loader.h` → `vmaf_dnn_verify_signature`), apply
  `VMAF_EXPORT` to the internal declaration instead. Verify after any
  structural change with:
  ```bash
  nm -D --defined-only build/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
  # Must print 0
  ```
  On upstream sync: any new `vmaf_*` entry point added upstream that
  the fork's headers re-export needs `VMAF_EXPORT` added in the same
  merge commit; missing it will silently hide the symbol.

- **Fuzz-harness coverage rule** (fork-local,
  [ADR-0270](../docs/adr/0270-fuzzing-scaffold.md) +
  [ADR-0311](../docs/adr/0311-libfuzzer-harness-expansion.md)): every
  attacker-reachable parser added under `libvmaf/tools/` must ship
  with a matching libFuzzer harness under
  [`test/fuzz/`](test/fuzz/) before merge — the convention is one
  `fuzz_<surface>.c` + a 3–6-seed corpus + a row in
  `test/fuzz/meson.build` and the
  [`.github/workflows/fuzz.yml`](../.github/workflows/fuzz.yml)
  nightly matrix. Three harnesses currently ship: `fuzz_y4m_input`
  (Y4M parser), `fuzz_yuv_input` (raw-YUV reader), `fuzz_cli_parse`
  (CLI argv tokeniser + colon-delimited model/feature parsers).
  The harnesses re-include `tools/{y4m_input,yuv_input,vidinput,
  cli_parse}.c` as build inputs (via the static-source path, not
  `libvmaf.so`); upstream sync that splits or renames any of those
  source files needs the corresponding `meson.build` source-list
  update *and* a 60-second smoke run per harness against the seed
  corpus. The `__wrap_exit` longjmp shim in `fuzz_cli_parse.c` is
  GNU-ld / lld-specific and ships with a `-Wl,--wrap=exit` link
  arg; document any platform expansion. A pre-commit hook
  enforcing the new-parser-needs-new-harness contract is *not*
  yet wired — it can be added later once at least 5 parsers carry
  harnesses.

Backend-specific orientation:

- [src/cuda/AGENTS.md](src/cuda/AGENTS.md) — CUDA backend runtime
- [src/sycl/AGENTS.md](src/sycl/AGENTS.md) — SYCL backend runtime
- [src/vulkan/AGENTS.md](src/vulkan/AGENTS.md) — Vulkan backend runtime
- [src/dnn/AGENTS.md](src/dnn/AGENTS.md) — ONNX Runtime integration (tiny AI)
- [src/feature/AGENTS.md](src/feature/AGENTS.md) — feature extractors + SIMD
- [test/AGENTS.md](test/AGENTS.md) — C unit tests

## Build

```bash
meson setup build [-Denable_cuda=true|false] [-Denable_sycl=true|false] [-Denable_dnn=auto]
ninja -C build
meson test -C build
```

Shortcut: `/build-vmaf --backend=cpu|cuda|sycl|all`.

## Backend-engagement foot-guns (read before benching)

Two CLI flags govern backend selection at runtime; the relationship is
**not** "set the flag for the backend you want". A run that looks like
it's exercising CUDA can silently fall through to CPU and still produce
the expected score (because CUDA extractors emit the same logical
features). Symptoms reviewers see: bit-exact CPU/CUDA/SYCL pools,
identical fps across backends — **always wrong on a non-trivial fixture
size unless the flags are right.**

- **`--gpumask` is a CUDA *disable* bitmask, not a device pin.**
  `compute_fex_flags` ([`src/libvmaf.c::compute_fex_flags`](src/libvmaf.c))
  enables the CUDA dispatch slot only when `gpumask == 0`. Any
  nonzero value disables CUDA. Public-header semantics:
  `if gpumask: disable CUDA` (see
  [`include/libvmaf/libvmaf.h`](include/libvmaf/libvmaf.h) `VmafConfiguration::gpumask`).
- **`--backend cuda` currently *initialises* CUDA but disables the
  dispatch slot.** The CLI sets `gpumask = 1` ([`tools/cli_parse.c::parse_cli_args`](tools/cli_parse.c))
  intending it as a device pin, but the runtime treats `gpumask = 1`
  as "disable CUDA". So `--backend cuda` runs CUDA init and then
  routes the actual feature extractors through the CPU path. The
  vmaf_v0.6.1-style models then emit identical scores because the
  CPU code is doing the work. **This is a bug** (tracked separately;
  do not assume `--backend cuda` works for benching until it lands).
- **`--no_cuda` / `--no_sycl` are *disable*-only.** Pairing
  `--no_sycl` alone (without `--gpumask`) does NOT enable CUDA — it
  just disables SYCL while leaving CUDA unrequested. The CLI inits
  CUDA only when `c.use_gpumask && !c.no_cuda` (see
  [`tools/vmaf.c`](tools/vmaf.c) device-init block).

**Correct invocations for backend bench / cross-backend diff:**

| intent | flags |
|---|---|
| CPU only | `--no_cuda --no_sycl` |
| CUDA | `--gpumask=0 --no_sycl` |
| SYCL | `--sycl_device=0 --no_cuda` |
| Vulkan | `--vulkan_device=N` (no `--no_cuda`/`--no_sycl` interaction) |

Verify CUDA actually engaged by inspecting the JSON `frames[0].metrics`
key set: CPU emits 14–15 keys (`integer_aim`, `integer_motion3`,
`integer_adm3` are CPU-only); CUDA emits 11–12 keys (the CPU-only
extras absent). Same-key-count + identical pool across two backends =
both ran the same code path.

The bench script `testdata/bench_all.sh` historically used the wrong
flag pattern (`--no_sycl` for "CUDA"). Numbers from runs older than
2026-04-28 in `docs/benchmarks.md` were CPU-on-CPU comparisons. See
[ADR-0064 in rebase-notes](../docs/rebase-notes.md) and PR #169 for
the corrected methodology.

- **Build-option combination validation** (fork-local, fixes 1b/1c/1d of audit-build-matrix-symbols-2026-05-16):
  `libvmaf/src/meson.build` validates dependent-option combinations and errors or warns when incompatible flags are set:
  — `enable_mcp_sse=enabled/true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_uds=true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_stdio=true` requires `enable_mcp=true` (error if violated);
  — `enable_avx512=true` with `enable_asm=false` issues a warning (no-op, not an error);
  — `enable_hipcc=true` with `enable_hip=false` issues a warning (no-op, not an error).
  The checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. The principle: every option that depends on another must `error()` on the bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](src/meson.build).
