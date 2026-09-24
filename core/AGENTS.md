<!-- markdownlint-disable MD013 MD060 -->
# AGENTS.md — core

Orientation for any coding agent working inside `core/`. Root orientation
lives in [../AGENTS.md](../AGENTS.md); this file is scoped hand-off for
this subtree. Claude Code equivalents in [../CLAUDE.md](../CLAUDE.md).

## Scope

C engine — VMAF metric, feature extractors, backends, public API,
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
- **Every new file starts with license header** (Netflix preserved on
  upstream-touched; Lusoris/Claude on wholly-new — see ADR-0025).
- **Fixed-width integer printf formatting uses `<inttypes.h>` PRI macros**
  (`PRId64`, `PRIu64`, `PRIx64`, `PRIu32`, …) — never `(unsigned long)` +
  `%lu` or `(long long)` + `%lld` for `uint64_t` / `int64_t`.
  `(unsigned long)` form silently truncates on Windows LLP64 (32-bit
  `unsigned long`); PRI macros expand correctly on every supported data
  model. CERT FIO47-C, MISRA 21.6, [ADR-0876](../docs/adr/0876-printf-format-portability-pri-macros.md).
  Non-fixed-width POSIX types (`off_t`, `pid_t`, `time_t`) keep
  `(long long)` + `%lld` cast idiom (or `(intmax_t)` + `%jd`); Windows
  `DWORD` keeps `(unsigned long)` + `%lu` (cast spells type
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
- [ADR-0012](../docs/adr/0012-coding-standards-jpl-cert-misra.md) — coding-standards stack.
- [ADR-0022](../docs/adr/0022-inference-runtime-onnx.md) — execution-provider mapping ORT↔backends.
- [ADR-0024](../docs/adr/0024-netflix-golden-preserved.md) — golden-data gate (three CPU reference pairs, never modified).
- [ADR-0025](../docs/adr/0025-copyright-handling-dual-notice.md) — dual-copyright policy.
- [ADR-0137](../docs/adr/0137-thread-local-locale-for-numeric-io.md) —
  thread-local locale abstraction (`thread_locale.h`) for all numeric I/O.

## Rebase-sensitive invariants

- **`meson_version` is pinned to `>= 1.4.0`, not upstream's value**
  (fork-local, ADR-0692 / T-CI-MESON-C23-APT-2026-08-30):
  [`meson.build`](meson.build) sets Meson's built-in fallback list
  `c_std=c23,c2x,c17,none`. Meson selects the first spelling supported by the
  active compiler; MSVC-syntax drivers then receive `/std:clatest` as a narrow
  override. Keep `none` last: Meson's `intel-llvm-cl` backend advertises only
  `c89`/`c99`/`c11`, so a list without a value every backend accepts aborts
  configure on the Windows MSVC+SYCL leg.
  `c23` is only recognised from Meson 1.4.0 onward (verified: 1.3.2 rejects
  it, 1.4.0 accepts it). Declared `meson_version` must stay at or above 1.4.0
  for as long as the fork keeps this standard policy. Upstream sync that
  rewrites `project()` will conflict here — keep the fork's
  `>= 1.4.0`. Lowering it does not fail loudly: build instead dies
  much later at configure with cryptic
  `ERROR: Unknown C std ['c23']`, which is what took out seven CI
  workflows at once when Ubuntu's meson 1.3.2 was still in use.
- **Feature-context deduplication includes parsed feature parameters**
  (ADR-0385; [Research-2047](../docs/research/2047-option-aware-context-registration-2026-09-08.md)):
  [`src/fex_ctx_vector.cpp`](src/fex_ctx_vector.cpp) compares
  `vmaf_feature_name_from_options()` keys for shared advertised feature bases.
  CPU/GPU twins with equivalent defaults or alias-spelled options still use
  first registration. Different feature parameters must coexist: matching
  only `provided_features[]` drops second model/explicit option set.
  When either list is absent, preserve extractor-name fallback with parsed
  options. Name-allocation failure returns `-ENOMEM` without consuming
  incoming context; growth failure preserves pointer table, count and
  capacity. Private capacity helper checks both `UINT_MAX` and `SIZE_MAX`
  before doubling, including release builds. Keep actual-motion public
  scoring test and Linux allocation-failure controls on rebase.
- **`picture_compute_geometry` stride alignment uses `unsigned` + `1u`
  mask** (fork-local, round-5 `-fsanitize=integer` sweep):
  `aligned_y` and `aligned_c` in
  [`src/picture.c`](src/picture.c) are declared `const unsigned` and
  bitmask uses `DATA_ALIGN - 1u` (not `DATA_ALIGN - 1`) so
  complement stays in unsigned domain, avoids signed→unsigned
  implicit conversion that fires with `-fsanitize=integer`. If
  upstream sync rewrites `picture_compute_geometry`, preserve
  `unsigned` type and `1u` literal. See
  [docs/rebase-notes.md](../docs/rebase-notes.md)
  §PR-fix-picture-align-unsigned-narrowing.
- **`vmaf_init` cpumask narrowing uses explicit `(unsigned)` cast**
  (fork-local, round-5 `-fsanitize=integer` sweep):
  `vmaf_set_cpu_flags_mask((unsigned)(~cfg.cpumask))` in
  [`src/libvmaf.c`](src/libvmaf.c). Cast is deliberate: all
  defined CPU flag bits fit in 6 bits; high 32 bits of
  `uint64_t cpumask` complement are always zero for any valid input.
  Never remove explicit cast.
- **Output writers return `ferror(outfile) ? -EIO : 0`.**
  `vmaf_write_output_{xml,json,csv,sub}` in
  [src/output.c](src/output.c) use single tail `return` that
  checks `ferror(outfile)` — per [ADR-0119](../docs/adr/0119-cli-precision-default-revert.md).
  Any upstream patch changing tail to bare `return 0`
  must be merged so fork's `ferror` check survives.
  Thread-locale bracket from [ADR-0137](../docs/adr/0137-thread-local-locale-for-numeric-io.md)
  is `push_c()` at entry → body → `pop()` before `ferror`
  check; dropping `pop()` leaks `locale_t` on POSIX,
  leaves calling thread locked to `"C"` on Windows.
- **JSON model loader has no fixed feature/knot schema ceiling.**
  [`src/read_json_model.c`](src/read_json_model.c) grows
  `VmafModel.feature` and `score_transform.knots.list` from JSON
  payload. Never restore old `MAX_FEATURE_COUNT` / `MAX_KNOT_COUNT`
  rejection pattern during upstream sync; external model JSONs with
  65+ features or 11+ piecewise-linear knots must parse if payload
  is otherwise valid. Regression coverage lives in
  [`test/test_model.c`](test/test_model.c).
- **HIP backend scaffold contract** (fork-local, ADR-0212 / T7-10):
  `enable_hip=true` build path compiles
  [src/hip/](src/hip/) and [src/feature/hip/](src/feature/hip/)
  into `libvmaf_feature_static_lib`, exposes public C-API
  entry points in
  [include/libvmaf/libvmaf_hip.h](include/libvmaf/libvmaf_hip.h)
  (`vmaf_hip_state_init` / `_import_state` / `_state_free` /
  `vmaf_hip_list_devices` / `vmaf_hip_available`). Until
  runtime PR (T7-10b) lands, every public entry point returns
  `-ENOSYS` and smoke test
  [test/test_hip_smoke.c](test/test_hip_smoke.c) pins that
  contract. Any rebase or refactor "succeeding" scaffold
  (e.g. accidentally enabling code path) without flipping
  smoke expectations breaks rebase story for runtime PR.
  `dependency('hip-lang')` probe in
  [src/hip/meson.build](src/hip/meson.build) stays
  `required: false` for scaffold; flipping to `true` belongs
  to runtime PR. `enable_hip` option type is
  `boolean` (matching `enable_cuda` / `enable_sycl`); never
  convert it to `feature` without ADR amendment per ADR-0212
  § "Decision".
- **Bounded thread-pool admission** (Netflix `8fc71e3`, fork lifetime adaptation):
  `src/thread_pool.c` admits at most one queued job per successfully created
  worker, before payload allocation. Keep dequeue wakeups and checked condition
  teardown. Destruction waits for both workers and already-blocked producers;
  waking producer does not make it safe to free its mutex. Preserve
  cancellation/lifetime and mixed-payload tests in
  `test/test_thread_pool_backpressure.c`. Callbacks must not enqueue into
  same pool. Serialize destruction against API entry, including mutex-acquisition
  waiters; only proven registered capacity waiters can be cancelled concurrently. See
  [thread-pool behavior](../docs/development/thread-pool.md).
- **Thread-pool job recycling + inline data buffer** (fork-local,
  ADR-0147): [`src/thread_pool.c`](src/thread_pool.c) recycles
  `VmafThreadPoolJob` slots via `pool->free_jobs` free list
  (mutex-protected by `queue.lock`), stores payloads ≤
  `JOB_INLINE_DATA_SIZE` (64 bytes) inside `job->inline_data`
  instead of second `malloc`. Cleanup path distinguishes
  inline from heap payloads via
  `job->data != job->inline_data` guard in
  `vmaf_thread_pool_job_clear_data`; never collapse this
  check during rebase — freeing `inline_data` would corrupt
  slot. Fork's `func(void *data, void **thread_data)`
  signature and `VmafThreadPoolWorker` per-worker-data path must
  survive any upstream sync; Netflix upstream PR #1464 (closed)
  has similar job-pool but uses bare
  `func(void *data)` signature — on conflict keep fork's
  two-arg signature, merge only pool-mechanics changes.
  Struct carries immutable `n_workers_created` field (written
  once in `pool_create`, never decremented) alongside live
  `n_threads` counter (decremented by each exiting runner thread under
  `queue.lock`). `destroy` reads `n_workers_created` — not `n_threads`
  — to iterate `workers[]` for `thread_data_free`; never collapse
  these two counters back into one during rebase or `destroy`
  path reacquires data race (C11 UB, TSan-detected). See
  [Research-0097](../docs/research/0097-thread-pool-pthread-create-unchecked-2026-05-10.md).
  See [ADR-0147](../docs/adr/0147-thread-pool-job-pool.md) and
  [rebase-notes 0040](../docs/rebase-notes.md).

- **`vmaf_picture_pool_fetch` error paths must always signal `pool->available`
  before unlocking** (fork-local, ADR-0960, round-25 audit A.2):
  [`src/picture_pool.c`](src/picture_pool.c) `return_to_pool` block
  must call `pthread_cond_signal(&pool->available)` every time index is
  pushed back to `pool->free_list`, regardless of whether push is from
  normal `vmaf_picture_unref` or from fetch error path. Omitting
  signal on error path creates deadlock: thread already in
  `pthread_cond_wait` (pool exhausted) will never wake. Invariant
  mirrors ADR-0607 (`feedback_shared_resource_outlive_worker_scope`):
  returning resource to pool must always notify waiters. Any rebase or
  refactor adding new `return_to_pool`-equivalent block must preserve
  signal. Covered by
  `test/test_picture_pool_error_paths.c::test_pool_waiter_woken_on_unref`.

- **`vmaf_fex_ctx_pool_create` has three-label cleanup chain**
  (fork-local, ADR-1060, r10 audit): `fail` → `free_p` → `free_fex_list`
  in `src/feature/feature_extractor.cpp`. Adding more allocations between
  `malloc(fex_list_sz)` and `pthread_mutex_init` needs corresponding label
  and goto. Prior two-label chain (`free_p` / `fail`) leaked `fex_list`
  on mutex-init failure.

- **`get_fex_list_entry` slot init is all-or-nothing** (fork-local,
  ADR-1060, r10 audit): `pthread_cond_init`, `ctx_list` malloc, and
  `vmaf_dictionary_copy` are all checked; any failure destroys cond,
  frees `ctx_list` before returning NULL. `pool->cnt` is NOT incremented on
  failure so partial slot is effectively invisible but still zeroed.
  Any rebase adding new resources to slot init sequence must add
  matching cleanup on early-return path.

- **`integer_vif` is luma-only across every backend** (fork-local,
  [ADR-0541](../docs/adr/0541-integer-vif-luma-only-clarification.md)).
  CPU [`src/feature/integer_vif.c`](src/feature/integer_vif.c) reads
  `data[0]` only, has no `enable_chroma` option; CUDA
  [`src/feature/cuda/integer_vif_cuda.c`](src/feature/cuda/integer_vif_cuda.c)
  hardcodes `s->n_planes = 1`, warn-on-trues `enable_chroma`; HIP,
  SYCL, Vulkan, Metal twins all match. Upstream Netflix/vmaf is
  same. VIF (Sheikh & Bovik, 2006) is defined on single luminance
  channel — multi-plane VIF has no MOS-correlation literature. Never
  "fix" `n_planes = 1` or "wire enable_chroma through" without
  filing fresh ADR including research digest and golden-data
  regeneration plan; previous attempt (PRs #948 + #949, 2026-05-16)
  was abandoned, left vestigial CUDA `enable_chroma` option as
  only artefact. Regression test
  [`test/test_integer_vif_cpu_cuda_parity.c`](test/test_integer_vif_cpu_cuda_parity.c)
  asserts CPU vs CUDA scale parity and `enable_chroma=true` bit-identity
  with default invocation — both must keep passing.

- **Vulkan PSNR chroma contract** (fork-local, [ADR-0216](../docs/adr/0216-vulkan-chroma-psnr.md)).
  [`src/feature/vulkan/psnr_vulkan.c`](src/feature/vulkan/psnr_vulkan.c)
  carries `ref_in[3] / dis_in[3] / se_partials[3]` arrays in
  `PsnrVulkanState` (Y / Cb / Cr), dispatches same
  `psnr.comp` shader once per active plane in single command
  buffer. Shader is plane-agnostic — reads
  `(width, height, num_workgroups_x)` from push constants. Rebases
  "simplifying" chroma loop back to single luma dispatch will
  silently regress `psnr_cb` / `psnr_cr` to CPU fall-through.
  This also breaks `cross_backend_vif_diff.py
  --feature psnr` gate, which now asserts on Y / Cb / Cr. YUV400
  is only supported `n_planes = 1` path; `pix_fmt`
  branch in `init` mirrors `enable_chroma = false` clamp in
  CPU `integer_psnr.c::init`, must follow it on any future
  `min_sse` / `psnr_max[p]` divergence. Descriptor pool is
  sized for 12 sets (4 frames in flight × 3 planes) — never
  shrink without re-checking lavapipe behaviour under
  frames-in-flight > 1.

- **PSNR `psnr_max` has two separate roles**
  (fork-local since [ADR-1193](../docs/adr/1193-psnr-uncapped-option.md)).
  Role (a): finite stand-in reported when `mse == 0` and true
  PSNR is `+inf` — unconditional, and what Netflix golden 60 / 84 /
  108 dB assertions pin. Role (b): truncation of computed values at
  same number — applied only when `uncapped` option is `false`.
  Upstream conflates the two in one
  `MIN(10*log10(peak^2 / MAX(mse, 1e-16)), psnr_max)`, so verbatim
  upstream hunk landing on `feature/integer_psnr.c::psnr_from_mse()`,
  `feature/float_psnr.c::extract()` or `feature/psnr.c::compute_psnr()`
  silently reintroduces Netflix/vmaf#1109. `!uncapped` arm is that
  upstream expression character-for-character and must stay that way:
  with `min_sse` below ~1.9e-11 ceiling rises past ~208 dB
  floored zero MSE produces, so re-derived `mse == 0 -> psnr_max`
  default would not be bit-identical there. Never merge two
  computed arms. `uncapped` option name,
  `VMAF_OPT_TYPE_BOOL` type and `false` default are mirrored across ten
  extractors — two CPU ones plus all eight GPU twins — must move
  together. It is deliberately **not**
  `VMAF_OPT_FLAG_FEATURE_PARAM`: CPU extractor appends without
  name dict while twins append with one, so flagging it would make
  backends emit different feature keys for same request.
  `core/test/test_psnr_uncapped.c` guards both directions (default
  must still report 60.0; `uncapped=true` must report 100.840479).
  Standing divergence, unchanged by that ADR: GPU twins implement
  only `enable_chroma` and `uncapped`; `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse` are CPU-only.

- **Embedded MCP runtime contract** (fork-local, [ADR-0209](../docs/adr/0209-mcp-embedded-scaffold.md)).
  [`src/mcp/`](src/mcp/) now contains promoted in-process MCP
  runtime declared in
  [`include/libvmaf/libvmaf_mcp.h`](include/libvmaf/libvmaf_mcp.h):
  stdio, UDS, and loopback-SSE transports, plus read-only
  `list_features` and out-of-band `compute_vmaf`. Preserve
  early argument validation (`-EINVAL` on NULLs / negative fds /
  NULL paths) before any runtime work; smoke tests for `_init`,
  `_start_uds`, `_start_stdio`, and `_start_sse` rely on that
  contract. `compute_vmaf` must keep using per-call ephemeral
  `VmafContext`, not host scorer, because pooled scoring commits
  models destructively. `enable_mcp` umbrella flag must default
  `false` until mutating measurement-thread tools and SPSC bridge
  land; silent-flip risk is same as ADR-0175's Vulkan
  precedent.

- **MS-SSIM `enable_lcs` GPU contract** (fork-local,
  [ADR-0243](../docs/adr/0243-enable-lcs-gpu.md)).
  [`src/feature/cuda/integer_ms_ssim_cuda.c`](src/feature/cuda/integer_ms_ssim_cuda.c)
  and
  [`src/feature/vulkan/ms_ssim_vulkan.c`](src/feature/vulkan/ms_ssim_vulkan.c)
  emit 15 extra metrics — `float_ms_ssim_{l,c,s}_scale{0..4}` —
  when `enable_lcs` option is true, mirroring CPU
  `float_ms_ssim` extractor in
  [`src/feature/float_ms_ssim.c`](src/feature/float_ms_ssim.c#L189-L221).
  Metric names, ordering (metric-wise — all `l_scale*` first,
  then `c_*`, then `s_*`), and `places=4` cross-backend contract
  are part of public API surface; never rename, reorder, or
  introduce per-backend variations. Kernels themselves
  (`ms_ssim_vert_lcs` CUDA / vert pass in `ms_ssim.comp` Vulkan)
  already compute per-scale `l_means[i]` / `c_means[i]` /
  `s_means[i]` doubles — gating only host-side
  `vmaf_feature_collector_append` calls keeps default-path
  (`enable_lcs=false`) output bit-identical to pre-T7-35
  binary. Cross-backend gate's `float_ms_ssim_lcs`
  pseudo-feature in
  [`scripts/ci/cross_backend_vif_diff.py`](../scripts/ci/cross_backend_vif_diff.py)
  and
  [`scripts/ci/cross_backend_parity_gate.py`](../scripts/ci/cross_backend_parity_gate.py)
  enforces contract; never drop `FEATURE_ALIASES` entry
  or matching `FEATURE_TOLERANCE` row on rebase.

- **GPU-parity matrix gate contract** (fork-local,
  [ADR-0214](../docs/adr/0214-gpu-parity-ci-gate.md)).
  [`scripts/ci/cross_backend_parity_gate.py`](../scripts/ci/cross_backend_parity_gate.py)
  is single source of truth for per-feature absolute
  tolerance every (CPU↔GPU, GPU↔GPU) cell must respect. CI
  job `vulkan-parity-matrix-gate` in
  [tests-and-quality-gates.yml](../.github/workflows/tests-and-quality-gates.yml)
  runs it on every PR over CPU↔Vulkan/lavapipe; CUDA/SYCL/hardware-
  Vulkan are advisory until self-hosted runner exists. Never
  tighten `FEATURE_TOLERANCE` entry without measurement-driven
  follow-up ADR (per CLAUDE.md §12 r1). Adding new feature with
  GPU twin requires (1) `FEATURE_METRICS` entry, (2)
  `FEATURE_TOLERANCE` entry if feature relaxes places=4, and
  (3) row in
  [`docs/development/cross-backend-gate.md`](../docs/development/cross-backend-gate.md).

- **`float_motion` extra-options surface (upstream port from Netflix
  b949cebf, 2026-04-29).** [`src/feature/float_motion.c`](src/feature/float_motion.c)
  exposes four extra options (`motion_add_scale1`, `motion_add_uv`,
  `motion_filter_size`, `motion_max_val`), emits `motion3_score` on
  second frame. Default Y-plane / scale-0 path stays bit-identical
  to pre-port baseline by routing through `compute_motion_simd()`
  (AVX2 / AVX-512 / NEON `float_sad_line` dispatch); non-default paths
  (`scale1`, UV) fall through to scalar `compute_motion()` in
  [`src/feature/motion.c`](src/feature/motion.c).
  `picture_copy()` / `picture_copy_hbd()` signature in
  [`src/feature/picture_copy.{c,h}`](src/feature/picture_copy.h) gained
  trailing `int channel` parameter (upstream d3647c73 prerequisite); every
  fork-local caller (`float_adm.c`, `float_moment.c`,
  `float_ms_ssim.c`, `float_psnr.c`, `float_ssim.c`, `float_vif.c`,
  `cuda/integer_ms_ssim_cuda.c`, `sycl/integer_ms_ssim_sycl.cpp`,
  `sycl/integer_ssim_sycl.cpp`, `vulkan/ms_ssim_vulkan.c`,
  `vulkan/ssim_vulkan.c`) passes `0` for Y-plane. On future upstream
  syncs, never drop SIMD fast-path wrapper: NASA/JPL Power-of-10
  inner-loop budget still demands it, and Netflix golden-data gate
  ([ADR-0024](../docs/adr/0024-netflix-golden-preserved.md)) is regression-
  flagging if default path stops dispatching to `vmaf_image_sad_avx2`
  / `_avx512` / `_neon`. See
  [`docs/rebase-notes.md` §0049](../docs/rebase-notes.md).

- **icpx-aware clang-tidy wrapper for SYCL TUs** (fork-local,
  [ADR-0217](../docs/adr/0217-sycl-toolchain-cleanup.md)).
  [`scripts/ci/clang-tidy-sycl.sh`](../scripts/ci/clang-tidy-sycl.sh)
  is single entry point for linting `core/src/sycl/**` and
  `core/src/feature/sycl/**` files; it injects oneAPI SYCL
  include path + `-D__SYCL_DEVICE_ONLY__=0` so stock LLVM clang-tidy
  resolves `<sycl/sycl.hpp>`. CI lane
  `Tidy SYCL` in
  [`.github/workflows/lint-and-format.yml`](../.github/workflows/lint-and-format.yml)
  runs wrapper over SYCL build tree; never invoke stock
  `clang-tidy` directly against SYCL TUs (will surface
  `'sycl/sycl.hpp' file not found` clang-diagnostic-errors). When
  adding new SYCL TU, no AGENTS.md update is needed — wrapper
  finds it via changed-file diff. Wrapper resolves icpx
  install via `$ICPX_ROOT` (override) or
  `/opt/intel/oneapi/compiler/latest` (default); if Intel
  reorganises this layout in future release, wrapper's candidate
  list needs new path added (see `for cand in ...` block in
  script). Companion bench-time helper:
  [`scripts/ci/sycl-bench-env.sh`](../scripts/ci/sycl-bench-env.sh).
- **GPU long-tail terminus reached** (fork-local, T7-36 closure
  via [ADR-0210](../docs/adr/0210-cambi-vulkan-integration.md)).
  Every registered feature extractor now has at least one GPU twin
  — cambi was last remaining gap. lpips remains ORT-delegated
  per [ADR-0022](../docs/adr/0022-inference-runtime-onnx.md).
  Adding new feature extractor without same-PR GPU twin is now
  explicit choice — record deferral in ADR body.
  Governing batches:
  [ADR-0182](../docs/adr/0182-gpu-long-tail-batch-1.md) (1) +
  [ADR-0188](../docs/adr/0188-gpu-long-tail-batch-2.md) (2) +
  [ADR-0192](../docs/adr/0192-gpu-long-tail-batch-3.md) (3).
- **`motion3_score` GPU contract (T3-15(c) / ADR-0219).** Three GPU
  motion twins (`src/feature/vulkan/motion_vulkan.c`,
  `src/feature/cuda/integer_motion_cuda.c`,
  `src/feature/sycl/integer_motion_sycl.cpp`) emit
  `VMAF_integer_feature_motion3_score` in 3-frame window mode by
  applying CPU's host-side post-process to motion2: `clip(motion_blend(
  motion2 * motion_fps_weight, motion_blend_factor,
  motion_blend_offset), motion_max_val)` with optional moving-average.
  No device-side state is added — motion3 is deterministic scalar
  function of motion2. Two invariants rebase story depends on:
  (1) `motion_five_frame_window=true` returns `-ENOTSUP` at `init()`
  (5-deep blur ring + second SAD pair are still deferred — never
  silently fall back to 3-frame path); (2) any Netflix
  upstream sync touching `motion_blend()` in
  [`motion_blend_tools.h`](src/feature/motion_blend_tools.h),
  `motion_max_val` clip, or moving-average rule MUST mirror
  change into `motion3_postprocess_*` across all three GPU files
  in same PR. Cross-backend parity gate at `places=4`
  (`scripts/ci/cross_backend_parity_gate.py` +
  `scripts/ci/cross_backend_vif_diff.py` `FEATURE_METRICS["motion"]`
  → `integer_motion3`) catches drift, but only after full GPU
  run. See [`docs/rebase-notes.md` §0219](../docs/rebase-notes.md).

- **Symbol visibility: every new public entry point needs `VMAF_EXPORT`**
  (fork-local, [ADR-0379](../docs/adr/0379-libvmaf-symbol-visibility.md) /
  Research-0092). `core/src/meson.build` compiles all TUs with
  `-fvisibility=hidden`; only symbols annotated with `VMAF_EXPORT`
  (defined in `core/include/libvmaf/macros.h`) appear in
  dynamic symbol table of `libvmaf.so`. When adding new public C
  entry point, apply `VMAF_EXPORT` to its declaration in installed
  public header. Attribute propagates from declaration to
  definition if definition TU includes header, so no annotation
  of definition itself is normally required. Exception: if
  definition TU does *not* include public header (see
  `src/dnn/model_loader.h` → `vmaf_dnn_verify_signature`), apply
  `VMAF_EXPORT` to internal declaration instead. Verify after any
  structural change with:

  ```bash
  nm -D --defined-only build/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
  # Must print 0
  ```

  On upstream sync: any new `vmaf_*` entry point added upstream that
  fork's headers re-export needs `VMAF_EXPORT` added in same
  merge commit; missing it will silently hide symbol.

- **Fuzz-harness coverage rule** (fork-local,
  [ADR-0270](../docs/adr/0270-fuzzing-scaffold.md) +
  [ADR-0311](../docs/adr/0311-libfuzzer-harness-expansion.md)): every
  attacker-reachable parser added under `core/tools/` must ship
  with matching libFuzzer harness under
  [`test/fuzz/`](test/fuzz/) before merge — convention is one
  `fuzz_<surface>.c` + 3–6-seed corpus + row in
  `test/fuzz/meson.build` and
  [`.github/workflows/fuzz.yml`](../.github/workflows/fuzz.yml)
  nightly matrix. Three harnesses currently ship: `fuzz_y4m_input`
  (Y4M parser), `fuzz_yuv_input` (raw-YUV reader), `fuzz_cli_parse`
  (CLI argv tokeniser + colon-delimited model/feature parsers).
  Harnesses re-include `tools/{y4m_input,yuv_input,vidinput,
  cli_parse}.c` as build inputs (via static-source path, not
  `libvmaf.so`); upstream sync splitting or renaming any of those
  source files needs corresponding `meson.build` source-list
  update *and* 60-second smoke run per harness against seed
  corpus. `__wrap_exit` longjmp shim in `fuzz_cli_parse.c` is
  GNU-ld / lld-specific, ships with `-Wl,--wrap=exit` link
  arg; document any platform expansion. Pre-commit hook
  enforcing new-parser-needs-new-harness contract is *not*
  yet wired — can be added later once at least 5 parsers carry
  harnesses.

- **Language standards are Meson built-in fallback lists** (ADR-1056,
  2026-06-04): `project()` owns `c_std=c23,c2x,c17,none` and
  `cpp_std=c++26,c++23,c++latest`. Do not restore manual `-std=` probing or
  injection: it bypasses Meson's compiler checks, duplicates flags, and emits
  configure-time warnings on current Meson. Callers may still override either
  built-in option. The trailing `none` is not optional — it is the only value
  every backend accepts, and `intel-llvm-cl` (icx-cl) reaches no other entry
  in the list. The only platform exception is the MSVC-style C driver: after
  Meson selects `c17` (cl.exe) or `none` (icx-cl), `/std:clatest` is appended
  both to project arguments and feature probes so the fork retains its
  newest-C contract.

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

Two CLI flags govern backend selection at runtime; relationship is
**not** "set flag for backend you want". A run that looks like
it's exercising CUDA can silently fall through to CPU and still produce
expected score (because CUDA extractors emit same logical
features). Symptoms reviewers see: bit-exact CPU/CUDA/SYCL pools,
identical fps across backends — **always wrong on non-trivial fixture
size unless flags are right.**

- **`--gpumask` is CUDA *disable* bitmask, not device pin.**
  `compute_fex_flags` ([`src/libvmaf.c::compute_fex_flags`](src/libvmaf.c))
  enables CUDA dispatch slot only when `gpumask == 0`. Any
  nonzero value disables CUDA. Public-header semantics:
  `if gpumask: disable CUDA` (see
  [`include/libvmaf/libvmaf.h`](include/libvmaf/libvmaf.h) `VmafConfiguration::gpumask`).
- **`--backend cuda` does engage CUDA — this bullet used to say it did
  not; that is no longer true.** Re-verified on 2026-09-06 at commit
  `cd52f2670` on `ryzen-4090-arc` host while refreshing baselines
  (ADR-1185): `--backend cuda` on 200-frame 4K BBB pair runs at
  167.16 fps against CPU's 14.37 fps, emits 14 `frames[0].metrics`
  keys against CPU's 15. Identical scores *and* identical fps would
  be fallback signature; neither holds. Use `--backend $name` as
  canonical exclusive selector. (Historical note, kept because it explains
  older bench rows: CLI once set `gpumask = 1` as device pin while
  runtime read any nonzero `gpumask` as "disable CUDA", so
  `--backend cuda` did in fact run CUDA init, then score on CPU.
  Bench numbers captured while that was live are not CUDA numbers.)
- **Check engagement per run, never trust flag.** Cheap check is
  `frames[0].metrics` key count, which `testdata/bench_backends.py`
  records for every cell. See
  [`docs/development/backend-perf-baselines.md`](../docs/development/backend-perf-baselines.md).
- **`--no_cuda` / `--no_sycl` are *disable*-only.** Pairing
  `--no_sycl` alone (without `--gpumask`) does NOT enable CUDA — it
  only disables SYCL while leaving CUDA unrequested. CLI inits
  CUDA only when `c.use_gpumask && !c.no_cuda` (see
  [`tools/vmaf.cpp`](tools/vmaf.cpp) device-init block).

**Correct invocations for backend bench / cross-backend diff:**

| intent | flags |
|---|---|
| CPU only | `--no_cuda --no_sycl` |
| CUDA | `--gpumask=0 --no_sycl` |
| SYCL | `--sycl_device=0 --no_cuda` |
| Vulkan | `--vulkan_device=N` (no `--no_cuda`/`--no_sycl` interaction) |

Verify CUDA engaged by inspecting JSON `frames[0].metrics`
key set: CPU emits 14–15 keys (`integer_aim`, `integer_motion3`,
`integer_adm3` are CPU-only); CUDA emits 11–12 keys (CPU-only
extras absent). Same-key-count + identical pool across two backends =
both ran same code path.

Bench script `testdata/bench_all.sh` historically used wrong
flag pattern (`--no_sycl` for "CUDA"). Numbers from runs older than
2026-04-28 in `docs/benchmarks.md` were CPU-on-CPU comparisons. See
[ADR-0064 in rebase-notes](../docs/rebase-notes.md) and PR #169 for
corrected methodology.

**A GPU bench row reading "unavailable" is not evidence backend is
absent** (measured 2026-09-06 on `cd52f2670`). Two invariants bench run must
hold onto:

- **`--threads N` aborts every GPU backend.** `--gpumask=0` or
  `--sycl_device=0` combined with any `--threads` value emits one
  `feature "VMAF_integer_feature_motion2_score" cannot be overwritten at index N`
  pair per frame, then `context could not be synchronized` /
  `problem flushing context`, exits 234 with no output file. Without
  `--threads` both backends succeed, are bit-stable over 10 runs (CUDA
  76.667830, SYCL 76.667746 on Netflix 576x324 pair). `bench_all.sh`
  hard-codes `--threads 1`, so its GPU rows fail by construction until
  `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06` closes — never silence that by
  deleting flag.
- **Never discard binary's stderr in bench harness.** `bench_all.sh` used
  to send it to `/dev/null`, relabel any non-zero exit as "backend likely
  unavailable"; that turned hard abort into row that looked like missing
  device for months. Capture stderr, print exit code, let reader
  decide what it means.

Key counts have moved since 2026-04 note above: on `cd52f2670` FFmpeg
filter path emits 15 keys for CPU, 14 for CUDA and 24 for SYCL (35 before
PR #1324). Use counts as "did backends run different code" signal,
not as fixed constants.

- **Build-option combination validation** (fork-local, fixes 1b/1c/1d of audit-build-matrix-symbols-2026-05-16):
  `core/src/meson.build` validates dependent-option combinations, errors or warns when incompatible flags are set:
  — `enable_mcp_sse=enabled/true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_uds=true` requires `enable_mcp=true` (error if violated);
  — `enable_mcp_stdio=true` requires `enable_mcp=true` (error if violated);
  — `enable_avx512=true` with `enable_asm=false` issues warning (no-op, not error);
  — `enable_hipcc=true` with `enable_hip=false` issues warning (no-op, not error).
  Checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. Principle: every option depending on another must `error()` on bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](src/meson.build).

- **C→C++23 conversion safety invariants** (adversarial review 2026-05-28,
  `docs/research/cpp23-wave-adversarial-review-20260528.md`):
  When converting `.c` TU to `.cpp` with `std::string_view` / `std::optional` /
  `std::unique_ptr` idioms, verify all of following before merging:

  1. **`string_view::data()` + C-string functions**: `strtol`, `strtod`, `strtof`,
     `strcmp`, `strlen`, `printf("%s", sv.data())` all require NUL-termination.
     If `string_view` is constructed from C-string literal or full C-string
     argument it is safe; if it could ever be substring slice, copy to `std::string`
     first or add `assert(sv.data()[sv.size()] == '\0')`.

  2. **`strtof` vs `strtod` precision**: returning `float` from `strtof`, assigning
     to `double` silently loses precision. If downstream use is `snprintf("%g", dv)`,
     output will be at `float` precision (~7 sig figs), not `double` (~15). Use
     `strtod` when result variable is `double`.

  3. **`make_unique` / `operator new` vs C-caller `free()`**: if struct is allocated
     by `std::make_unique` (uses `operator new`) but C callers may also call `free()`
     on same pointer (e.g. pre-existing teardown paths), this is UB / heap
     corruption. Document in header that `operator delete` (via `vmaf_ref_close`
     or equivalent) is ONLY valid deallocator; search all C callers for direct
     `free(ptr)` on that type.

  4. **`strlen(x) - N` unsigned underflow**: subtracting integer from `size_t`
     (returned by `strlen`) when `strlen(x) < N` wraps to huge value. Always
     check `strlen(x) >= N` first, or use `(len >= N ? len - N : 0)`.

  5. **Recursion in converted code**: Power of 10 rule 1 (no recursion) applies
     equally to `.cpp` files. `mkdirp` is known violator; future conversions must
     replace recursive path-splitting with iterative approach.

  6. **`[[nodiscard]]` on declarations vs definitions**: placing `[[nodiscard]]` only
     on `.cpp` definition without mirroring it in `extern "C"` declaration in
     header means C++ callers seeing only header will not get diagnostic.
     Always add `[[nodiscard]]` to header declaration (inside `extern "C"` block
     — C compilers silently ignore attribute).

  7. **Isolated C++ static libs carry NO `cpp_std` override (epic #1241 cleanup)**:
     `gpu_dispatch_env_cpp23_lib` (ADR-0858), `metadata_handler_cpp20_lib` (ADR-0708),
     `log_cpp23_lib`, `opt_cpp23_lib`, `picture_pool_cpp23_lib`, `gpu_picture_pool_cpp23_lib`,
     `read_json_model_cpp23_lib`, `libvmaf_cpu_static_lib`, `vmaf` / `vmafx` tools,
     `test_cli_parse*` / `test_picture_pool_cpp_error_paths` tests and `fuzz_cli_parse` are all
     compiled at the project-wide C++ standard selected by Meson's built-in
     preference list (ADR-1003 / ADR-1273). Former
     `override_options : ['cpp_std=...']` entries (and the
     `libvmaf_cpu_cpp_std` token variable) created conflicting duplicate flags.
     Never re-add per-target `cpp_std` overrides for new
     `.c → .cpp` conversions; do keep isolated-lib + `extract_all_objects` link pattern
     (it is what test targets consume). Only `override_options` remaining are
     `b_lto=false` ones (AVX-512 symbol visibility, macOS `test_output`) and those are real.

- **Required-aggregator invariant — `float_ansnr` removal (PR #38 / ADR-0865):**
  `float_ansnr` was deliberately removed from all backends (CPU, CUDA, HIP, SYCL,
  Metal, Vulkan) in PR #38. Following must remain consistent on any rebase
  or upstream-sync touching these files:
  - `core/test/test_hip_smoke.c`: `test_float_ansnr_hip_extractor_registered`
    function and its `test_table[]` entry have been removed. Never restore them
    without also restoring HIP extractor source.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~478):
    `VmafIntegerFeatureExtractor._generate_result()` must NOT list `float_ansnr`
    in its features. If upstream Netflix/vmaf adds `float_ansnr` back, re-add it
    in dedicated PR with CI verification.
  - `compat/python-vmaf/core/feature_extractor.py` (line ~463):
    `VmafIntegerFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT` must NOT
    map `"ansnr"` to `"float_ansnr"` while C library lacks extractor.
  Legacy path (`VmafFeatureExtractor`, line ~301) retains mapping as
  documented debt — tracked as T-LEGACY-RUNNER-ANSNR-BROKEN in `docs/state.md`.
  Checks run at configuration time (before `subdir()` calls) to catch misconfigurations early. Principle: every option depending on another must `error()` on bad combo, never silently no-op. See [`src/meson.build` lines 100–111, 74–76, 142–144](src/meson.build).

## Performance benchmark invariant (ADR-0752)

- **`testdata/perf_multi_resolution.json` is versioned performance baseline.**
  Any PR claiming performance improvement (CPU/CUDA/SYCL throughput, latency)
  must re-run `scripts/perf/bench-multi-resolution.sh` with same
  `--backends` and `--resolutions` flags. Include structured diff table
  in PR description (see `docs/development/perf.md §Comparing a PR against
  the baseline`).
- If PR intentionally changes throughput (optimisation or trade-off),
  commit updated `testdata/perf_multi_resolution.json` with justification
  in commit message.
- Upscaled fixture files (`testdata/ref_1920x1080_48f.yuv`, `testdata/ref_2560x1440_48f.yuv`,
  `testdata/dis_1920x1080_48f.yuv`, `testdata/dis_2560x1440_48f.yuv`) are
  generated on first run, are **not committed** (reproducible via
  `ffmpeg -vf scale=W:H:flags=bilinear` from in-tree 576×324 fixture).

## AVX-512 motion parity test invariant (ADR-0854)

- `core/test/test_motion_avx512_parity.c` provides direct bit-exact unit tests
  for all six AVX-512 motion kernels. If any of following functions is
  modified, corresponding test case **must** be re-run, must pass:
  - `motion_score_pipeline_8_avx512` (motion_v2_avx512.c)
  - `motion_score_pipeline_16_avx512` (motion_v2_avx512.c)
  - `sad_avx512` (motion_avx512.c)
  - `y_convolution_8_avx512` (motion_avx512.c)
  - `y_convolution_16_avx512` (motion_avx512.c)
  - `x_convolution_16_avx512` (motion_avx512.c)
- Scalar reference implementations in test file are line-for-line
  mirrors of production scalar paths. If scalar production path
  is changed (e.g. rounding bias, filter constants), update test's
  scalar reference accordingly, regenerate expected values.
- Test skips on hosts without `VMAF_X86_CPU_FLAG_AVX512`; this is
  intentional and correct. CI must run on AVX-512-capable host (see
  `.github/workflows/build.yml` x86_64 runner) for tests to be
  meaningful.

## Rebase-sensitive invariants (2026-06-04)

- **`vmaf_fex_integer_motion_v2` registration**: CPU extractor
  `vmaf_fex_integer_motion_v2` (from `feature/integer_motion_v2.c`) MUST
  appear in `feature_extractor_list[]` in `feature_extractor.cpp`. Removing
  it breaks `vmaf_get_feature_extractor_by_name("motion_v2")` on all CPU
  builds. Comment claiming this symbol was "merged into v1" in
  `feature_extractor.cpp` was incorrect, has been removed.

- **`context_extract` prev_ref management**: `vmaf_feature_extractor_context_extract()`
  updates `fex->prev_ref` after successful extract when extractor
  carries `VMAF_FEATURE_EXTRACTOR_PREV_REF`. `vmaf_feature_extractor_context_destroy()`
  releases held reference. Any refactor of these functions must preserve
  this pairing so direct callers (unit tests, pool code) observe same
  prev_ref semantics as `vmaf_read_pictures()`.

- **`predict_load_feature_score` EAGAIN vs EINVAL**: when feature vector
  is absent from collector (i.e., `fv == NULL` and
  `vmaf_feature_collector_get_score` returns `-EINVAL`), `predict_load_feature_score`
  must return `-EAGAIN`, not `-EINVAL`. This preserves Netflix#755 / ADR-0154:
  "score not yet written" is transient; only genuine programmer error
  (bad range, NULL pointer) returns `-EINVAL` from `vmaf_score_pooled`.

- **Float VIF must NOT dispatch to AVX-512 (ADR-1104)**: `vif_filter1d_s`,
  `vif_filter1d_sq_s`, and `vif_filter1d_xy_s` in `core/src/feature/vif_tools.c`
  dispatch to AVX2 or scalar only. AVX-512 float convolution path
  (`convolution_f32_avx512_{s,sq_s,xy_s}`) produces different IEEE-754 rounding
  than AVX2 (wider 512-bit FMA partial-sum tree), causing Netflix golden
  VMAFEXEC assertion (`76.66740433333332`, `places=4`) to fail on AVX-512 CPUs.
  Any future PR re-adding `#if HAVE_AVX512` dispatch to these three functions
  must demonstrate golden assertion still passes on AVX-512 hardware,
  must update ADR-1104. Integer VIF AVX-512 path (`vif_avx512.c`) is
  unaffected, must remain enabled.

## Rebase-sensitive invariants (2026-09-02, c-rework-core)

- **`vmaf_read_pictures` picture ownership is centralised in
  `ReadPicturesFrame` helpers** (`src/libvmaf.c`). Four helpers:
  `read_pictures_frame_translate` does CUDA host/device translation.
  `read_pictures_frame_select_host` hands host copies to DNN / worker
  pool only when `HW_FLAG_HOST` is set — zeroed `ref_host` on
  device-only path must never be dereferenced. `read_pictures_frame_cleanup`
  covers every non-batched exit. `read_pictures_frame_cleanup_after_batch`
  is device-only release after `threaded_read_pictures_batch` already unref'd
  host pictures (PR #838). Only `#ifdef HAVE_CUDA` left inside
  `vmaf_read_pictures` guards `read_pictures_frame_translate` call.
  Helper exists only in CUDA builds (CPU no-op stub would leave
  provably-dead error branch that cppcheck flags). Never re-inline further
  backend blocks into `vmaf_read_pictures`; add branches to matching
  helper.
- **Three cited `cppcheck-suppress constParameterPointer` markers** are
  deliberate, not debt: `vmaf_context_get_backend` (public ABI prototype in
  `include/libvmaf/libvmaf.h` is frozen), `read_pictures_validate_and_prep`
  (`vmaf_sycl_shared_frame_upload()` takes mutable pictures on SYCL
  build cppcheck never analyses) and `vmaf_feature_collector_unmount_model`
  (the public C declaration fixes the mutable model-pointer signature in
  `feature/feature_collector.cpp`). Drop marker only when its cited constraint
  is gone. `vmaf_feature_collector_get()` (`libvmaf_priv.h`) takes
  `const VmafContext *` — keep declaration and definition in step.
- **PREV_REF references are released only through `fex_release_prev_ref()`**
  and every CPU-pool skip decision goes through `batch_extractor_skip()` /
  `read_pictures_should_skip()`, which share `fex_subsample_skip()`. Two
  skip predicates must agree on which extractors worker pool runs, or
  extractor is dispatched twice (collector double-write) or never.
- **`vmaf_ctx_subsystems_init` owns init/teardown chain** for framesync →
  feature collector → extractor vector → thread pools; new subsystem gets
  new label in that function, not in `vmaf_init`.
- **C translation units keep `NULL`** (ADR-1138): `libvmaf.c` and `predict.c`
  carry file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket. Never rewrite `NULL` to
  `nullptr` in C sources (MSVC `/std:clatest` does not document it; upstream
  parity), keep `NOLINTEND` line at end of file when appending code.
- **Authoritative twin sides for model and unit tests (ADR-1153)**:
  `core/src/model.c` is sole authoritative implementation of model-loading
  and collection APIs; `model.cpp` was deleted as dead and stale. In `core/test/`,
  `test_dict.cpp` and `test_feature.cpp` are sole authoritative tests;
  uncompiled legacy C twins `test_dict.c` and `test_feature.c` were deleted.

## The default model has exactly one definition

`VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h` is only
place fork decides which model to score with when caller names none
(ADR-1168). Never write `"vmaf_v0.6.1"` as fallback anywhere else:

- C / C++ compiled against headers use macro.
- Anything linking libvmaf at runtime calls `vmaf_default_model_version()`.
- Go and Python tools use their gate-checked mirrors
  (`pkg/model.DefaultVersion`, `vmaftune.defaultmodel.DEFAULT_MODEL`,
  `vmafroiscore.defaultmodel.DEFAULT_MODEL`).

`scripts/ci/check-default-model-single-source.sh` fails build on drifted
mirror or new hardcoded fallback, so this is enforced rather than advisory.

**Rebase-sensitive:** macro and accessor do not exist upstream. AOM CTC
preset in `core/tools/cli_parse.cpp` deliberately keeps literal
`"vmaf_v0.6.1"` with `vmaf-model-pin:` comment because CTC specification
mandates that exact model. Upstream sync reverting either of those breaks
gate. See `docs/rebase-notes.md`.

**Changing value is more than a text edit.** One Netflix golden assertion,
`vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`, pins
default model's scores, so any change of default breaks it and ADR-0024
forbids editing it. Read `docs/development/default-model.md` before touching
value.

## The default model is `vmaf_v1.0.16_3d0h`, and NEG is not

Since [ADR-1169](../docs/adr/1169-default-model-v1-0-16.md) fork scores with
`vmaf_v1.0.16_3d0h` when no model is named. **Upstream Netflix still defaults to
`vmaf_v0.6.1`**, so upstream sync will look like it wants to revert this. It
does not. See `docs/rebase-notes.md`.

Two things easy to get wrong:

- **NEG is not derived from default.** There is no NEG counterpart to any
  `vmaf_v1.0.16_*` model — Netflix published NEG for v0.6.1 family only.
  `DefaultNEGVersion` / `DEFAULT_MODEL_NEG` are independent constants naming
  `vmaf_v0.6.1neg`. Writing `DefaultVersion + "neg"` synthesises
  `vmaf_v1.0.16_3d0hneg`, which does not exist and which libvmaf rejects at
  load. Python mirror *did* derive it that way, had to be fixed.
- **A default change breaks golden test by KeyError, not by value drift.**
  `vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`
  asserts v0.6.1 feature-family values (`vif_scale0..3`, `motion2`); v1
  family emits `integer_aim` / `cambi` / `speed_chroma` and none of those. Changing
  default again surfaces
  `KeyError('VMAFEXEC_vif_scale0_score')`. Resolve by naming model in
  that test, exactly as ADR-1169 did — **never** by editing
  `assertAlmostEqual` value (ADR-0024).

## GPU SpEED-chroma: singular is not an error (ADR-1202)

`core/src/feature/speed.c` overloads single `int` to mean *singular
covariance matrix*: `solve_covariance_system()` returns `cannot_invert`,
`speed_extract_score()` forwards it, and `extract_fex()` reads it to impute
`uv` score from whichever chroma channel inverted. That is CPU contract and
it is correct there.

**The GPU twins do not work that way.** In
`core/src/feature/cuda/speed_chroma_cuda.c`,
`core/src/feature/sycl/speed_chroma_sycl.cpp` and
`core/src/feature/hip/speed_chroma_hip.c`, linear-algebra helper handles
singularity itself (warn, zero solution, return 0), reserves its return
value for hard API failures. Singularity travels out through explicit
`bool *singular_out`.

Never "simplify" that out-parameter away by keying imputation off
return value again. That is what code did before ADR-1202, meant
real device error was routed into singular path: one channel failing
imputed from other, and both channels failing fell through to
`(0 + 0) * 0.5`, appended three `0.0` scores, returned success. A
`CUDA_ERROR_INVALID_VALUE` on every 4K frame therefore surfaced as pooled
VMAF 3.4 points off CPU score on exit-0 run, not as error.

Two related invariants in same files:

- **`SC_SOLVE_WARPS_PER_BLOCK` bounds block size; block count scales.**
  Backward-substitution launch maps one warp per linear system. Deriving
  block size from system count instead of block count exceeds
  CUDA's 1024-thread block limit above 256 systems — every 4K frame. SYCL
  and HIP twins already compute this correctly; keep three consistent.
- **A channel with exactly one singular side scores 0**, matching
  `speed_extract_score()`. Averaging in zeroed solution instead produces
  inflated score.

GPU parity tests in `meson test --suite=fast` all run below 256-system
threshold, cannot catch either invariant. Check 4K agreement against CPU
backend by hand. See `docs/rebase-notes.md` entry
`fix/cuda-speed-chroma-4k-launch`.

## Registration option-copy ownership (Research-2048)

In `src/libvmaf.c`, `fex_options_copy()` must release any partial destination
when `vmaf_dictionary_copy()` fails. Dictionary implementation can allocate
some entries before returning error. Every caller starts with independent
NULL destination: explicit registration, model registration and worker-context
creation. Preserve supplied-dictionary consumption guards of
`vmaf_use_feature()`; model and worker source dictionaries remain borrowed.
`fex_ctx_create_owned_options()` transfers only private copy on success,
releases it when context creation rejects options or fails allocation.

Keep public-only rejection regression separate from Linux ELF
partial-copy control: only latter intentionally interposes dictionary copy.
It forwards normal calls to actual shared library, injects real partial
destination, then checks error propagation and registration retry. Never link
private engine objects into these targets or invalidate old-library control.
Four DNN bridge exports retain out-of-profile consumers in
`src/dnn/dnn_attach_api.c`; `vmaf_ctx_dnn_has_session` and
`vmaf_register_metadata_handler` remain declared integration scaffolds in
`src/dnn/dnn_ctx.h` and `src/metadata.h`. Their six exact unused-function markers
preserve those interfaces without disabling ordinary unused checks. glibc
weak symbol retains its dictated ABI spelling. See
[Research-2048](../docs/research/2048-model-registration-ownership-2026-09-08.md).

## Every `NOLINT` names its ADR, and the citation has to be within one line

[ADR-0141](../docs/adr/0141-touched-file-cleanup-rule.md) §2 requires each
suppression to cite load-bearing invariant that forces it, in format
[ADR-0278](../docs/adr/0278-t7-5-nolint-sweep.md) fixed. Since CPU-lane
sweep (epic #1237) `core/src`, `core/tools` and `core/test` are at **zero**
uncited markers, and `scripts/ci/tidy-ratchet.py` measures that.

Three mechanics bite when adding or moving suppression:

- **Window is ±1 line.** `tidy-ratchet.py::count_uncited_nolints` accepts
  `ADR-NNNN` on previous line, same line, next line, or anywhere
  inside `/* … */` block comment that *holds* marker. An ADR named three
  lines above in separate comment block does **not** count — that is exactly
  how nine survivors of first sweep pass slipped through.
- **A long trailing comment can move marker off its diagnostic.** Appending
  citation to `free(p); // NOLINT(...)` can push line past 100-column
  budget. clang-format then wraps the *code*, leaving `// NOLINT` on
  continuation line while clang-tidy still reports finding at `free`
  token — silently dead suppression. When line no longer fits, convert to
  preceding `// NOLINTNEXTLINE(...) — ADR-NNNN` instead of letting
  clang-format re-wrap. `core/src/ref.cpp`, `core/src/opt.cpp` and
  `core/src/dnn/model_loader.c` are in that form for this reason.
- **A `NOLINTNEXTLINE` justification must never wrap onto second comment
  line.** Directive applies to line immediately following it. So
  `// NOLINTNEXTLINE(readability-function-size) — ADR-0141 §2 /` followed by
  `// ADR-0159 …: reason` points suppression at *comment*, not at
  function — diagnostic comes back. Ratchet still counts marker
  as cited, so citation gate stays green while warning count regresses.
  Only thing that catches it is `clang-tidy -p build <file>` against
  merge base. Keep directive on one line (short `— ADR-NNNN` suffix fits
  inside 100 columns), put prose in block comment above it;
  `core/src/feature/{x86,arm64}/psnr_hvs_*.c` and
  `core/src/feature/{x86,arm64}/ssimulacra2_host_*.c` are in that form for this
  reason.
- **Word "NOLINT" in prose is counted as marker.** `NOLINT_RE` matches
  bare token, so comment saying "NOLINT justification: …" registers as
  uncited suppression even when no directive exists. Write "suppression
  justification" instead.

Cite ADR that governs site: upstream-parity → ADR-0141 §2;
SIMD bit-exactness → ADR-0138 / ADR-0139 / ADR-0159 / ADR-0161 / ADR-0252;
public-ABI `const_cast` in C++23 pilot → ADR-0721.
