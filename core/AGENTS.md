<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core

Orientation for any coding agent working inside `core/`. Root orientation
lives in [../AGENTS.md](../AGENTS.md); this file is the scoped hand-off for
this subtree. Claude Code equivalents in [../CLAUDE.md](../CLAUDE.md).

## Scope

The C engine — VMAF metric, feature extractors, backends, public API,
CLI (`tools/vmaf`, `tools/vmaf_bench`), and C unit tests.

```text
core/
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
  `Copyright 2026 Lusoris` for wholly-new files.
  See [ADR-0025](../docs/adr/0025-copyright-handling-dual-notice.md).
- **Style**: K&R, 4-space, 100-char columns, `.clang-format` authoritative.
- **Banned functions** (see `docs/principles.md §1.2 rule 30`): `gets`,
  `strcpy`, `strcat`, `sprintf`, `strtok`, `atoi`, `atof`, `rand`, `system`.
- **Every non-void return value is checked or explicitly `(void)`-discarded.**
- **Every new file starts with the license header** (Netflix preserved on
  upstream-touched; Lusoris/Claude on wholly-new — see ADR-0025).
- **Fixed-width integer printf formatting uses `<inttypes.h>` PRI macros**
  (`PRId64`, `PRIu64`, `PRIx64`, `PRIu32`, …) — never `(unsigned long)` +
  `%lu` or `(long long)` + `%lld` for `uint64_t` / `int64_t`. The
  `(unsigned long)` form silently truncates on Windows LLP64 (32-bit
  `unsigned long`); PRI macros expand correctly on every supported data
  model. CERT FIO47-C, MISRA 21.6, [ADR-0876](../docs/adr/0876-printf-format-portability-pri-macros.md).
  Non-fixed-width POSIX types (`off_t`, `pid_t`, `time_t`) keep the
  `(long long)` + `%lld` cast idiom (or `(intmax_t)` + `%jd`); Windows
  `DWORD` keeps `(unsigned long)` + `%lu` (the cast spells the type
  exactly).

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

- **`meson_version` is pinned to `>= 1.4.0`, not upstream's value**
  (fork-local, ADR-0692 / T-CI-MESON-C23-APT-2026-08-30):
  [`meson.build`](meson.build) sets `default_options: ['c_std=c23', …]`,
  and `c23` is only a recognised `c_std` value from meson 1.4.0 onward
  (verified: 1.3.2 rejects it, 1.4.0 accepts it). The declared
  `meson_version` must stay at or above 1.4.0 for as long as the fork
  keeps `c_std=c23`, which upstream does not set. An upstream sync that
  rewrites the `project()` call will conflict here — keep the fork's
  `>= 1.4.0`. Lowering it does not fail loudly: the build instead dies
  much later at configure with the cryptic
  `ERROR: Unknown C std ['c23']`, which is what took out seven CI
  workflows at once when Ubuntu's meson 1.3.2 was still in use.
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

- **`vmaf_picture_pool_fetch` error paths must always signal `pool->available`
  before unlocking** (fork-local, ADR-0960, round-25 audit A.2):
  [`src/picture_pool.c`](src/picture_pool.c) `return_to_pool` block
  must call `pthread_cond_signal(&pool->available)` every time an index is
  pushed back to `pool->free_list`, regardless of whether the push is from
  a normal `vmaf_picture_unref` or from a fetch error path. Omitting the
  signal on the error path creates a deadlock: a thread already in
  `pthread_cond_wait` (pool exhausted) will never wake. The invariant
  mirrors ADR-0607 (`feedback_shared_resource_outlive_worker_scope`):
  returning a resource to a pool must always notify waiters. Any rebase or
  refactor that adds a new `return_to_pool`-equivalent block must preserve
  the signal. Covered by
  `test/test_picture_pool_error_paths.c::test_pool_waiter_woken_on_unref`.

- **`vmaf_fex_ctx_pool_create` has a three-label cleanup chain**
  (fork-local, ADR-1060, r10 audit): `fail` → `free_p` → `free_fex_list`
  in `src/feature/feature_extractor.cpp`. If you add more allocations between
  `malloc(fex_list_sz)` and `pthread_mutex_init`, add a corresponding label
  and goto. The prior two-label chain (`free_p` / `fail`) leaked `fex_list`
  on mutex-init failure.

- **`get_fex_list_entry` slot init is all-or-nothing** (fork-local,
  ADR-1060, r10 audit): `pthread_cond_init`, `ctx_list` malloc, and
  `vmaf_dictionary_copy` are all checked; any failure destroys the cond and
  frees `ctx_list` before returning NULL. `pool->cnt` is NOT incremented on
  failure so the partial slot is effectively invisible but still zeroed.
  Any rebase that adds new resources to the slot init sequence must add a
  matching cleanup on the early-return path.

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

- **PSNR `psnr_max` has two separate roles**
  (fork-local since [ADR-1193](../docs/adr/1193-psnr-uncapped-option.md)).
  Role (a): the finite stand-in reported when `mse == 0` and the true
  PSNR is `+inf` — unconditional, and what the Netflix golden 60 / 84 /
  108 dB assertions pin. Role (b): the truncation of computed values at
  the same number — applied only when the `uncapped` option is `false`.
  Upstream conflates the two in one
  `MIN(10*log10(peak^2 / MAX(mse, 1e-16)), psnr_max)`, so a verbatim
  upstream hunk landing on `feature/integer_psnr.c::psnr_from_mse()`,
  `feature/float_psnr.c::extract()` or `feature/psnr.c::compute_psnr()`
  silently reintroduces Netflix/vmaf#1109. The `!uncapped` arm is that
  upstream expression character-for-character and must stay that way:
  with a `min_sse` below ~1.9e-11 the ceiling rises past the ~208 dB a
  floored zero MSE produces, so a re-derived `mse == 0 -> psnr_max`
  default would not be bit-identical there. Do not merge the two
  computed arms. The `uncapped` option name,
  `VMAF_OPT_TYPE_BOOL` type and `false` default are mirrored across ten
  extractors — the two CPU ones plus all eight GPU twins — and must move
  together. It is deliberately **not**
  `VMAF_OPT_FLAG_FEATURE_PARAM`: the CPU extractor appends without a
  name dict while the twins append with one, so flagging it would make
  the backends emit different feature keys for the same request.
  `core/test/test_psnr_uncapped.c` guards both directions (the default
  must still report 60.0; `uncapped=true` must report 100.840479).
  Standing divergence, unchanged by that ADR: the GPU twins implement
  only `enable_chroma` and `uncapped`; `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse` are CPU-only.

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
  fork-local caller (`float_adm.c`, `float_moment.c`,
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
  is the single entry point for linting `core/src/sycl/**` and
  `core/src/feature/sycl/**` files; it injects the oneAPI SYCL
  include path + `-D__SYCL_DEVICE_ONLY__=0` so stock LLVM clang-tidy
  resolves `<sycl/sycl.hpp>`. The CI lane
  `Tidy SYCL (advisory)` in
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
  Research-0092). `core/src/meson.build` compiles all TUs with
  `-fvisibility=hidden`; only symbols annotated with `VMAF_EXPORT`
  (defined in `core/include/libvmaf/macros.h`) appear in the
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
  attacker-reachable parser added under `core/tools/` must ship
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

- **`meson.build` C++ standard is injected via `add_project_arguments`, not
  `default_options`** (ADR-1056, 2026-06-04): `cpp_std=c++23` was removed
  from `default_options` because meson's MSVC backend rejects `c++23` (it
  only accepts `c++11/14/17/20/vc++latest`). The standard is now set by an
  `if get_option('cpp_std') == 'none'` block that injects `/std:c++latest`
  on MSVC and `-std=c++23` on everything else. Three invariants that must
  survive any rebase:
  (1) the guard condition `get_option('cpp_std') == 'none'` must be
  preserved — removing it causes the SYCL leg's `-Dcpp_std=c++latest` override
  to conflict with the injected `-std=c++23` flag;
  (2) the block must remain between `cxx = meson.get_compiler('cpp')` and
  the first `cc.check_header` call so the compiler is defined before the
  branch;
  (3) do not re-add `cpp_std=c++23` to `default_options` — the meson
  configure step will fail on any Windows + MSVC matrix leg.

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
- **`--backend cuda` does engage CUDA — this bullet used to say it did
  not, and that is no longer true.** Re-verified on 2026-09-06 at commit
  `cd52f2670` on the `ryzen-4090-arc` host while refreshing the baselines
  (ADR-1185): `--backend cuda` on the 200-frame 4K BBB pair runs at
  167.16 fps against the CPU's 14.37 fps and emits 14 `frames[0].metrics`
  keys against the CPU's 15. Identical scores *and* identical fps would
  be the fallback signature; neither holds. Use `--backend $name` as the
  canonical exclusive selector. (Historical note, kept because it explains
  older bench rows: the CLI once set `gpumask = 1` as a device pin while
  the runtime read any nonzero `gpumask` as "disable CUDA", so
  `--backend cuda` really did run CUDA init and then score on the CPU.
  Bench numbers captured while that was live are not CUDA numbers.)
- **Check engagement per run, do not trust the flag.** The cheap check is
  the `frames[0].metrics` key count, which `testdata/bench_backends.py`
  records for every cell. See
  [`docs/development/backend-perf-baselines.md`](../docs/development/backend-perf-baselines.md).
- **`--no_cuda` / `--no_sycl` are *disable*-only.** Pairing
  `--no_sycl` alone (without `--gpumask`) does NOT enable CUDA — it
  just disables SYCL while leaving CUDA unrequested. The CLI inits
  CUDA only when `c.use_gpumask && !c.no_cuda` (see
  [`tools/vmaf.cpp`](tools/vmaf.cpp) device-init block).

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

**A GPU bench row that reads "unavailable" is not evidence the backend is
absent** (measured 2026-09-06 on `cd52f2670`). Two invariants a bench run must
hold onto:

- **`--threads N` aborts every GPU backend.** `--gpumask=0` or
  `--sycl_device=0` combined with any `--threads` value emits one
  `feature "VMAF_integer_feature_motion2_score" cannot be overwritten at index N`
  pair per frame, then `context could not be synchronized` /
  `problem flushing context`, and exits 234 with no output file. Without
  `--threads` both backends succeed and are bit-stable over 10 runs (CUDA
  76.667830, SYCL 76.667746 on the Netflix 576x324 pair). `bench_all.sh`
  hard-codes `--threads 1`, so its GPU rows fail by construction until
  `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06` closes — do not silence that by
  deleting the flag.
- **Never discard the binary's stderr in a bench harness.** `bench_all.sh` used
  to send it to `/dev/null` and relabel any non-zero exit as "backend likely
  unavailable"; that turned a hard abort into a row that looked like a missing
  device for months. Capture stderr, print the exit code, and let the reader
  decide what it means.

Key counts have moved since the 2026-04 note above: on `cd52f2670` the FFmpeg
filter path emits 15 keys for CPU, 14 for CUDA and 24 for SYCL (35 before
PR #1324). Use the counts as a "did the backends run different code" signal,
not as fixed constants.

- **Build-option combination validation** (fork-local, fixes 1b/1c/1d of audit-build-matrix-symbols-2026-05-16):
  `core/src/meson.build` validates dependent-option combinations and errors or warns when incompatible flags are set:
  — `enable_mcp_sse=enabled/true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_uds=true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_stdio=true` requires `enable_mcp=true` (error if violated);
  — `enable_avx512=true` with `enable_asm=false` issues a warning (no-op, not an error);
  — `enable_hipcc=true` with `enable_hip=false` issues a warning (no-op, not an error).
  The checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. The principle: every option that depends on another must `error()` on the bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](src/meson.build).

- **C→C++23 conversion safety invariants** (adversarial review 2026-05-28,
  `docs/research/cpp23-wave-adversarial-review-20260528.md`):
  When converting a `.c` TU to `.cpp` with `std::string_view` / `std::optional` /
  `std::unique_ptr` idioms, verify all of the following before merging:

  1. **`string_view::data()` + C-string functions**: `strtol`, `strtod`, `strtof`,
     `strcmp`, `strlen`, `printf("%s", sv.data())` all require NUL-termination.
     If the `string_view` is constructed from a C-string literal or a full C-string
     argument it is safe; if it could ever be a substring slice, copy to `std::string`
     first or add `assert(sv.data()[sv.size()] == '\0')`.

  2. **`strtof` vs `strtod` precision**: returning `float` from `strtof` and assigning
     to `double` silently loses precision. If the downstream use is `snprintf("%g", dv)`
     the output will be at `float` precision (~7 sig figs), not `double` (~15). Use
     `strtod` when the result variable is `double`.

  3. **`make_unique` / `operator new` vs C-caller `free()`**: if a struct is allocated
     by `std::make_unique` (uses `operator new`) but C callers may also call `free()`
     on the same pointer (e.g. pre-existing teardown paths), this is UB / heap
     corruption. Document in the header that `operator delete` (via `vmaf_ref_close`
     or equivalent) is the ONLY valid deallocator; search all C callers for direct
     `free(ptr)` on that type.

  4. **`strlen(x) - N` unsigned underflow**: subtracting an integer from `size_t`
     (returned by `strlen`) when `strlen(x) < N` wraps to a huge value. Always
     check `strlen(x) >= N` first, or use `(len >= N ? len - N : 0)`.

  5. **Recursion in converted code**: the Power of 10 rule 1 (no recursion) applies
     equally to `.cpp` files. `mkdirp` is the known violator; future conversions must
     replace recursive path-splitting with an iterative approach.

  6. **`[[nodiscard]]` on declarations vs definitions**: placing `[[nodiscard]]` only
     on the `.cpp` definition without mirroring it in the `extern "C"` declaration in
     the header means C++ callers that only see the header will not get the diagnostic.
     Always add `[[nodiscard]]` to the header declaration (inside the `extern "C"` block
     — C compilers silently ignore the attribute).

  7. **`gpu_dispatch_env.cpp` isolated lib pattern (ADR-0858)**: `gpu_dispatch_env.cpp`
     is compiled as `gpu_dispatch_env_cpp23_lib` with `override_options: ['cpp_std=c++23']`
     and linked into `libvmaf` via `extract_all_objects`. The project default is now
     `cpp_std=c++23` (ADR-1003), so the isolated-lib `override_options` are redundant but
     harmless; cleanup is deferred. Do NOT collapse the isolated libs back into
     `libvmaf_sources` until the redundancy cleanup PR lands, to preserve a clean git
     history for the `extract_all_objects` pattern. Always follow the
     `metadata_handler_cpp20_lib` (ADR-0708) pattern for any further `.c → .cpp`
     conversions.

- **Required-aggregator invariant — `float_ansnr` removal (PR #38 / ADR-0865):**
  `float_ansnr` was deliberately removed from all backends (CPU, CUDA, HIP, SYCL,
  Metal, Vulkan) in PR #38. The following must remain consistent on any rebase
  or upstream-sync that touches these files:
  - `core/test/test_hip_smoke.c`: the `test_float_ansnr_hip_extractor_registered`
    function and its `test_table[]` entry have been removed. Do NOT restore them
    without also restoring the HIP extractor source.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~478):
    `VmafIntegerFeatureExtractor._generate_result()` must NOT list `float_ansnr`
    in its features. If upstream Netflix/vmaf adds `float_ansnr` back, re-add it
    in a dedicated PR with CI verification.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~463):
    `VmafIntegerFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT` must NOT
    map `"ansnr"` to `"float_ansnr"` while the C library lacks the extractor.
  The legacy path (`VmafFeatureExtractor`, line ~301) retains the mapping as
  documented debt — tracked as T-LEGACY-RUNNER-ANSNR-BROKEN in `docs/state.md`.
  The checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. The principle: every option that depends on another must `error()` on the bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](src/meson.build).

## Performance benchmark invariant (ADR-0752)

- **`testdata/perf_multi_resolution.json` is the versioned performance baseline.**
  Any PR that claims a performance improvement (CPU/CUDA/SYCL throughput, latency)
  must re-run `scripts/perf/bench-multi-resolution.sh` with the same
  `--backends` and `--resolutions` flags and include a structured diff table
  in the PR description (see `docs/development/perf.md §Comparing a PR against
  the baseline`).
- If the PR intentionally changes throughput (optimisation or trade-off),
  commit the updated `testdata/perf_multi_resolution.json` with the justification
  in the commit message.
- Upscaled fixture files (`testdata/ref_1920x1080_48f.yuv`, `testdata/ref_2560x1440_48f.yuv`,
  `testdata/dis_1920x1080_48f.yuv`, `testdata/dis_2560x1440_48f.yuv`) are
  generated on first run and are **not committed** (they are reproducible via
  `ffmpeg -vf scale=W:H:flags=bilinear` from the in-tree 576×324 fixture).

## AVX-512 motion parity test invariant (ADR-0854)

- `core/test/test_motion_avx512_parity.c` provides direct bit-exact unit tests
  for all six AVX-512 motion kernels.  If any of the following functions is
  modified, the corresponding test case **must** be re-run and must pass:
  - `motion_score_pipeline_8_avx512` (motion_v2_avx512.c)
  - `motion_score_pipeline_16_avx512` (motion_v2_avx512.c)
  - `sad_avx512` (motion_avx512.c)
  - `y_convolution_8_avx512` (motion_avx512.c)
  - `y_convolution_16_avx512` (motion_avx512.c)
  - `x_convolution_16_avx512` (motion_avx512.c)
- The scalar reference implementations in the test file are line-for-line
  mirrors of the production scalar paths.  If the scalar production path
  is changed (e.g. rounding bias, filter constants), update the test's
  scalar reference accordingly and regenerate expected values.
- The test skips on hosts without `VMAF_X86_CPU_FLAG_AVX512`; this is
  intentional and correct.  CI must run on an AVX-512-capable host (see
  `.github/workflows/build.yml` x86_64 runner) for the tests to be
  meaningful.

## Rebase-sensitive invariants (2026-06-04)

- **`vmaf_fex_integer_motion_v2` registration**: the CPU extractor
  `vmaf_fex_integer_motion_v2` (from `feature/integer_motion_v2.c`) MUST
  appear in `feature_extractor_list[]` in `feature_extractor.cpp`.  Removing
  it breaks `vmaf_get_feature_extractor_by_name("motion_v2")` on all CPU
  builds.  The comment that claimed this symbol was "merged into v1" in
  `feature_extractor.cpp` was incorrect and has been removed.

- **`context_extract` prev_ref management**: `vmaf_feature_extractor_context_extract()`
  updates `fex->prev_ref` after a successful extract when the extractor
  carries `VMAF_FEATURE_EXTRACTOR_PREV_REF`.  `vmaf_feature_extractor_context_destroy()`
  releases the held reference.  Any refactor of these functions must preserve
  this pairing so direct callers (unit tests, pool code) observe the same
  prev_ref semantics as `vmaf_read_pictures()`.

- **`predict_load_feature_score` EAGAIN vs EINVAL**: when the feature vector
  is absent from the collector (i.e., `fv == NULL` and
  `vmaf_feature_collector_get_score` returns `-EINVAL`), `predict_load_feature_score`
  must return `-EAGAIN`, not `-EINVAL`.  This preserves Netflix#755 / ADR-0154:
  "score not yet written" is transient; only genuine programmer error
  (bad range, NULL pointer) returns `-EINVAL` from `vmaf_score_pooled`.

- **Float VIF must NOT dispatch to AVX-512 (ADR-1104)**: `vif_filter1d_s`,
  `vif_filter1d_sq_s`, and `vif_filter1d_xy_s` in `core/src/feature/vif_tools.c`
  dispatch to AVX2 or scalar only.  The AVX-512 float convolution path
  (`convolution_f32_avx512_{s,sq_s,xy_s}`) produces different IEEE-754 rounding
  than AVX2 (wider 512-bit FMA partial-sum tree), causing the Netflix golden
  VMAFEXEC assertion (`76.66740433333332`, `places=4`) to fail on AVX-512 CPUs.
  Any future PR that re-adds `#if HAVE_AVX512` dispatch to these three functions
  must demonstrate that the golden assertion still passes on AVX-512 hardware
  and must update ADR-1104.  The integer VIF AVX-512 path (`vif_avx512.c`) is
  unaffected and must remain enabled.

## Rebase-sensitive invariants (2026-09-02, c-rework-core)

- **`vmaf_read_pictures` picture ownership is centralised in the
  `ReadPicturesFrame` helpers** (`src/libvmaf.c`):
  `read_pictures_frame_translate` (CUDA host/device translation),
  `read_pictures_frame_select_host` (hand host copies to the DNN / worker
  pool only when `HW_FLAG_HOST` is set — the zeroed `ref_host` on the
  device-only path must never be dereferenced), `read_pictures_frame_cleanup`
  (every non-batched exit) and `read_pictures_frame_cleanup_after_batch`
  (device-only release after `threaded_read_pictures_batch` already unref'd
  the host pictures — PR #838). The only `#ifdef HAVE_CUDA` left inside
  `vmaf_read_pictures` guards the `read_pictures_frame_translate` call: the
  helper exists only in CUDA builds (a CPU no-op stub would leave a
  provably-dead error branch that cppcheck flags). Do not re-inline further
  backend blocks into `vmaf_read_pictures`; add branches to the matching
  helper.
- **Three cited `cppcheck-suppress constParameterPointer` markers** are
  deliberate, not debt: `vmaf_context_get_backend` (public ABI prototype in
  `include/libvmaf/libvmaf.h` is frozen), `read_pictures_validate_and_prep`
  (`vmaf_sycl_shared_frame_upload()` takes mutable pictures on the SYCL
  build cppcheck never analyses) and `vmaf_feature_collector_unmount_model`
  (prototype shared with the C++ twin `feature/feature_collector.cpp`, which
  `test_predict` compiles). Drop a marker only when its cited constraint is
  gone. `vmaf_feature_collector_get()` (`libvmaf_priv.h`) takes a
  `const VmafContext *` — keep the declaration and definition in step.
- **PREV_REF references are released only through `fex_release_prev_ref()`**
  and every CPU-pool skip decision goes through `batch_extractor_skip()` /
  `read_pictures_should_skip()`, which share `fex_subsample_skip()`. The two
  skip predicates must agree on which extractors the worker pool runs, or an
  extractor is dispatched twice (collector double-write) or never.
- **`vmaf_ctx_subsystems_init` owns the init/teardown chain** for framesync →
  feature collector → extractor vector → thread pools; a new subsystem gets a
  new label in that function, not in `vmaf_init`.
- **C translation units keep `NULL`** (ADR-1138): `libvmaf.c`, `predict.c`
  and `feature/feature_collector.c` carry a file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket. Do not rewrite `NULL` to
  `nullptr` in C sources (MSVC `/std:clatest` does not document it; upstream
  parity), and keep the `NOLINTEND` line at end of file when appending code.
- **Authoritative twin sides for model and unit tests (ADR-1153)**:
  `core/src/model.c` is the sole authoritative implementation of the model-loading
  and collection APIs; `model.cpp` was deleted as dead and stale. In `core/test/`,
  `test_dict.cpp` and `test_feature.cpp` are the sole authoritative tests;
  the uncompiled legacy C twins `test_dict.c` and `test_feature.c` were deleted.

## The default model has exactly one definition

`VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h` is the only
place the fork decides which model to score with when the caller names none
(ADR-1168). Do not write `"vmaf_v0.6.1"` as a fallback anywhere else:

- C / C++ compiled against the headers use the macro.
- Anything linking libvmaf at runtime calls `vmaf_default_model_version()`.
- Go and the Python tools use their gate-checked mirrors
  (`pkg/model.DefaultVersion`, `vmaftune.defaultmodel.DEFAULT_MODEL`,
  `vmafroiscore.defaultmodel.DEFAULT_MODEL`).

`scripts/ci/check-default-model-single-source.sh` fails the build on a drifted
mirror or a new hardcoded fallback, so this is enforced rather than advisory.

**Rebase-sensitive:** the macro and the accessor do not exist upstream, and the
AOM CTC preset in `core/tools/cli_parse.cpp` deliberately keeps the literal
`"vmaf_v0.6.1"` with a `vmaf-model-pin:` comment because the CTC specification
mandates that exact model. An upstream sync that reverts either of those breaks
the gate. See `docs/rebase-notes.md`.

**Changing the value is not just an edit.** One Netflix golden assertion,
`vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`, pins
the default model's scores, so any change of default breaks it and ADR-0024
forbids editing it. Read `docs/development/default-model.md` before touching
the value.

## The default model is `vmaf_v1.0.16_3d0h`, and NEG is not

Since [ADR-1169](../docs/adr/1169-default-model-v1-0-16.md) the fork scores with
`vmaf_v1.0.16_3d0h` when no model is named. **Upstream Netflix still defaults to
`vmaf_v0.6.1`**, so an upstream sync will look like it wants to revert this. It
does not. See `docs/rebase-notes.md`.

Two things that are easy to get wrong:

- **NEG is not derived from the default.** There is no NEG counterpart to any
  `vmaf_v1.0.16_*` model — Netflix published NEG for the v0.6.1 family only.
  `DefaultNEGVersion` / `DEFAULT_MODEL_NEG` are independent constants naming
  `vmaf_v0.6.1neg`. Writing `DefaultVersion + "neg"` synthesises
  `vmaf_v1.0.16_3d0hneg`, which does not exist and which libvmaf rejects at
  load. The Python mirror *did* derive it that way and had to be fixed.
- **A default change breaks a golden test by KeyError, not by value drift.**
  `vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`
  asserts v0.6.1 feature-family values (`vif_scale0..3`, `motion2`); the v1
  family emits `integer_aim` / `cambi` / `speed_chroma` and none of those. If you
  change the default again you will see
  `KeyError('VMAFEXEC_vif_scale0_score')`. Resolve it by naming the model in
  that test, exactly as ADR-1169 did — **never** by editing an
  `assertAlmostEqual` value (ADR-0024).
