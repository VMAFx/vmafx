<!-- markdownlint-disable MD013 -->
# HIP Feature Extractors — Invariant Notes

Parent: [../AGENTS.md](../AGENTS.md). HIP backend runtime lives at
[`../../hip/AGENTS.md`](../../hip/AGENTS.md); CUDA sibling =
[`../cuda/AGENTS.md`](../cuda/AGENTS.md).

## Deleted orphan/dead TUs (ADR-0546)

Following files removed from this directory by ADR-0546
(`chore/hip-cuda-orphan-tu-cleanup`, 2026-05-18):

- `adm_hip.c` — defined `vmaf_hip_adm_{init,run,destroy}` stubs
  (`init` returned 0, `run` returned -ENOSYS); no
  `VmafFeatureExtractor` registration; zero callers in repo.
  API-level HIP ADM covered by `integer_adm_hip.c`
  (`vmaf_fex_integer_adm_hip`).
- `motion_hip.c` — same pattern; `vmaf_hip_motion_{init,run,destroy}`;
  covered by `integer_motion_hip.c` and `float_motion_hip.c`.
- `vif_hip.c` — same pattern; `vmaf_hip_vif_{init,run,destroy}`;
  covered by `integer_vif_hip.c` and `float_vif_hip.c`.
- `feature_hip.h` — forward-declared only above three triplets;
  removed with last of its consumers.

Also removed from `core/src/feature/hip/`:

- `adm_decouple.hip` (in `integer_adm/`) — dead uncompiled file
  removed by ADR-1154; decoupling already inlined in `adm_csf.hip`.
- `integer_moment_hip.h` and `integer_moment/moment_score.hip` —
  orphan header and duplicate kernel removed by ADR-1154; canonical
  implementation = `float_moment_hip.c` using
  `float_moment/moment_score.hip`.
- `integer_ciede_hip.c` — duplicate of `ciede_hip.c`; both defined
  `vmaf_fex_ciede_hip`. Only `ciede_hip.c` in `hip/meson.build`.
- `integer_moment_hip.c` — duplicate of `float_moment_hip.c`; both
  defined `vmaf_fex_float_moment_hip`. Only `float_moment_hip.c` in
  `hip/meson.build`.

And from `core/src/feature/cuda/`:

- `float_ssim_cuda.c` — stale copy superseded by
  `integer_ssim_cuda.c`; both defined `vmaf_fex_float_ssim_cuda`.
  Only `integer_ssim_cuda.c` in `core/src/meson.build`. Newer TU adds
  `enable_chroma` and other improvements missing from orphan copy.

Do not re-add any of these files without first consulting ADR-0546 /
ADR-1154.

## Memory copy direction enum discipline

Every `hipMemcpy*` call's direction enum **must match actual memory
placement** of source and destination pointers:

- `hipMemcpyHostToDevice`: source = host-accessible (CPU pointer), destination = device-side
- `hipMemcpyDeviceToHost`: source = device-side, destination = host-accessible (CPU or pinned)
- `hipMemcpyDeviceToDevice`: source and destination both device-side

Mismatches = undefined behavior on some HIP runtimes; may silently
corrupt results or trigger runtime faults.

**Established patterns:**

- Picture planes arrive from VMAF pipeline as CPU-side `VmafPicture` structs with `data[0..2]` pointers (host memory). Copying these into device-allocated staging buffers requires `hipMemcpyHostToDevice`.
- Readback buffers allocated via `hipHostMalloc` in `src/hip/kernel_template.c` = host-pinned memory, safe to use with `hipMemcpyDeviceToHost` for kernel output collection.

See 2026-05-16 GPU audit (no follow-up ADR filed; invariant stands on
its own; relevant `hipMemcpy*` direction tags in `integer_psnr_hip.c`
now at lines 212 / 359 / 364 post-refactor).

## Kernel-arg pattern for `hipModuleLaunchKernel` pointer parameters (ADR-0537)

`__global__` kernel taking pointer parameter (e.g.
`const uint16_t *vif_filt_dev`) -> corresponding entry in host
`void *args[]` array
must be **address of variable holding device pointer**. NOT device
pointer value itself, and NOT address of host memory.

```c
/* CORRECT — &dev_ptr_var points to the variable storing the device ptr */
void *dev_ptr = s->some_dev_malloc;
void *args[] = { /* …, */ &dev_ptr, /* … */ };
hipModuleLaunchKernel(func, …, args, NULL);

/* WRONG — passes a host address that the GPU will dereference */
void *args[] = { /* …, */ (void *)host_static_array, /* … */ };

/* WRONG — passes the pointer value into the position where the HIP
 * runtime expects the address-of-pointer */
void *args[] = { /* …, */ s->some_dev_malloc, /* … */ };
```

Pre-ADR-0537 `integer_vif_hip.c` had second form for filter table
parameter, which AMD GPU dereferenced and faulted on with "Memory
access fault by GPU node-1 ... Reason: Page not present or supervisor
privilege" — on first frame, before any score produced.

## Static const tables must be uploaded to device memory (ADR-0537)

Host-side `static const` array (e.g. `vif_filter1d_table[4][18]`
from `feature/integer_vif.h`) needing to be readable from HIP kernel
-> allocate device buffer at init time, `hipMemcpy(...,
hipMemcpyHostToDevice)` table contents once. Don't try passing host
address into kernel via `args[]` — WILL fault.

Cost = ~150 bytes one-shot at init, amortised across extractor's
lifetime. Established precedent: ADR-0537 in
`integer_vif_hip.c::init_fex_hip()`.

## Kernel name-suffix convention does NOT encode filter half-width (ADR-0537)

CUDA-port kernel-name suffixes like
`filter1d_8_vertical_kernel_uint32_t_17_9` or
`filter1d_16_vertical_kernel_uint2_3_0_3` encode
`(fwidth_0, fwidth_1, scale)` — *full filter widths* for main filter and rd
downsample filter, plus scale index. NOT half-widths.

Correct filter half-widths come from `vif_filter1d_width[scale] / 2`:

| Scale | `fwidth` | `half_width` |
|-------|----------|--------------|
| 0     | 17       | 8            |
| 1     | 9        | 4            |
| 2     | 5        | 2            |
| 3     | 3        | 1            |

Pre-ADR-0537 `integer_vif/vif_statistics.hip` used
`HALF = 9 / 5 / 3 / 0` (parsed from suffix), read 19 / 11 / 7 / 1 filter coefficients
per output pixel from 18-entry table — out-of-bounds reads.

## Scalar-per-thread is the correctness baseline; templated tiled is the perf goal

Porting CUDA twin to HIP -> write kernel scalar-per-thread first (no
shared-memory tiling, no warp reductions), confirm cross-backend
parity at `places=4` (ADR-0214) on Netflix golden pair *before*
porting perf optimisations. HIP wavefront sizes differ between RDNA
(32) and GCN/CDNA (64); warp-reduce path needs own tuning even after
scalar kernel is bit-exact.

**Boundary condition invariant (ADR-1103)**: all filter-loop boundary
reads must use `mirror2_i(idx, dim)` (two-bounce symmetric reflect),
**not** `clamp_i(idx, 0, dim-1)` (replicate-edge). CPU reference uses
`PADDING_SQ_DATA` (symmetric reflect at 0 and dim-1); `clamp_i`
disagrees with this for `filter_half_width` pixels at each edge,
producing places~2.75 gap (max |HIP−CPU| ≈ 0.0018) that violates
ADR-0214. `mirror2_i` already defined in
`integer_vif/vif_statistics.hip`; copy or re-derive it in any new HIP
filter kernel before adding boundary reads.

Established precedent: ADR-0537 ports
`integer_vif/vif_statistics.hip` scalar-per-thread (~540 lines vs
CUDA twin's ~850), accepts ~5–10× perf regression vs CUDA in
exchange for verifiable kernel surface. Perf optimisation deferred to
follow-up ADR.

## HSACO symbol naming — kernel keys must match the host-TU consumer (ADR-0539)

HIP host TU referencing kernel module via
`hipModuleLoadData(..., <name>_hsaco)` -> `hip_kernel_sources` meson key MUST be exactly
`<name>` — `xxd -i -n <name>_hsaco` step inside meson custom_target
derives symbol from that key. Two gotchas:

1. **Meson key matches symbol name directly**:
   `float_moment_hip.c` consumes `moment_score_hsaco` via:

   ```meson
   # float_moment_hip.c consumes `moment_score_hsaco`
   'moment_score' : feature_src_dir + 'hip/float_moment/moment_score.hip',
   ```

   (Historical `integer_moment_score` duplicate key removed in
   ADR-1154 together with uncompiled
   `integer_moment/moment_score.hip` orphan).

2. **Missing meson registration produces undefined-reference link
   error** for `<name>_hsaco`, NOT runtime `-ENOSYS`. Seeing such
   link failure -> either register kernel (preferred) or add weak
   stub in `hip_hsaco_stubs.c` (per ADR-0536 — only for kernels that
   can't yet compile standalone via `hipcc --genco`).

## Remove the weak HSACO stub the moment a real .hip lands (ADR-0539)

`.hip` kernel under `feature/hip/<extractor>/` becoming
standalone-buildable, registered in `hip_kernel_sources` in
`core/src/meson.build` -> **also delete matching
`VMAF_HSACO_WEAK_STUB(<extractor>_score_hsaco)` line from
`hip_hsaco_stubs.c` in same PR.** Leaving stub creates two
definitions of same symbol — strong xxd-embedded blob and weak
1-byte fallback. Linker resolves to strong one, but at cost of
`-Wlto-type-mismatch` warnings on every build. User direction = "no
stubs anywhere" once real kernel exists.

Pattern (ADR-0539 example for `float_vif_score`):

1. Confirm `.hip` source compiles via `hipcc --genco` in container
   (`ninja -C <build> src/<name>.hsaco`).
2. Remove `VMAF_HSACO_WEAK_STUB(<name>_hsaco)` line from
   `hip_hsaco_stubs.c`. Leave one-line comment citing ADR so
   reviewer sees why slot is gone.
3. Rebuild with `enable_hipcc=true`, grep ninja output for warnings
   referencing symbol — none should remain.

## IEEE-strict kernels go in `hip_cu_extra_flags` (ADR-0539)

HIP kernel relying on IEEE-754 add/mul ordering — e.g. any recursive
IIR (SSIMULACRA2 FastGaussian cascade), angle-flag reductions, or
numerically-sensitive variance / covariance combines. Add entry to
`hip_cu_extra_flags` dict in `core/src/meson.build` with
`['-ffp-contract=off']` (or richer flag list as needed).

hipcc / amdclang++ default to `-ffp-contract=fast` on device side,
silently fusing `n2 * sum - d1 * prev` patterns into FMAs, shifting
recursion past places=2 vs CPU / Vulkan `precise` twin. Mirrors CUDA
`cuda_cu_extra_flags` dict in same file. Current entries:
`ssimulacra2_blur`. Rebase invariant: porting new CUDA kernel listing
`--fmad=false` / `-ffp-contract=off` in `cuda_cu_extra_flags` -> add
matching HIP entry in same PR.

## Per-thread atomicAdd replaces CUDA per-warp `__shfl_down_sync` reduce (ADR-0539)

CUDA twin's `cuda_helper.cuh::warp_reduce` hard-codes
`warpSize == 32` in `__shfl_down_sync(0xffffffff, …)` mask. AMD wavefronts **64
wide** on every GCN / CDNA / RDNA target we ship to (gfx906 / gfx90a
/ gfx10 / gfx11); CUDA shuffle pattern incorrect on AMD even when
`__shfl_down_sync` available.

**Established pattern** when porting CUDA kernel ending in
`warp_reduce(accum) + per-warp atomicAdd`:

1. Drop warp reduce entirely.
2. Have **every thread** call `atomicAdd((uint64_cu *)&accum_global[band], lane_value)`.
3. Bit-exact w.r.t. CUDA twin since unsigned 64-bit integer addition
   associative and commutative — only reduction *order* changes.
4. Works on every AMD wavefront width without `#ifdef`-ing per arch.

`atomicAdd` on `unsigned long long` native on gfx90a / gfx10 /
gfx11, falls back to CAS loop on older GCN — HIP runtime handles
arch selection.

Precedents: `integer_vif/vif_statistics.hip` (ADR-0537),
`integer_adm/adm_csf_den.hip` + `integer_adm/adm_cm.hip` (ADR-0539).

## ADM `_hsaco` weak-stub slots have been removed (ADR-0539)

`hip_hsaco_stubs.c` weak fallbacks for `adm_dwt2_hsaco`,
`adm_csf_hsaco`, `adm_csf_den_hsaco`, `adm_cm_hsaco` have been
**removed** — four `.hip` kernels now build standalone via
`hipcc --genco` (registered in `core/src/meson.build::hip_kernel_sources`);
their xxd-embedded strong symbols supply blobs host TU loads.

Future ADM PR re-introducing CUDA-only helper into one of four
kernels (re-breaking standalone build) -> do NOT re-add weak stub —
fix kernel. Falling back to weak stubs silently degrades HIP ADM to
CPU at runtime (`hipModuleLoadData` call returns non-zero on empty
blob, extractor returns `-ENOSYS` from `init()`). User directive "no
stubs anywhere" explicitly rules this out.

`VMAF_HSACO_WEAK_STUB` macro in `hip_hsaco_stubs.c` retained as
documented pattern for in-progress ports of *new* extractors;
currently used by zero extractors.

## AdmBufferHip struct-by-value kernel parameters — P1 known issue (Research-0755)

`AdmBufferHip` (defined in `integer_adm_hip.h:70–96`) = ~272-byte
struct containing 6 DWT band sub-structs (each 4 device pointers)
plus 8 additional device-pointer fields. Currently passed by value
in multiple `__global__` kernel signatures in
`integer_adm/adm_csf.hip` and `integer_adm/adm_cm.hip`.

Mirrors PR #93 F3 finding on CUDA side. Consequences:

- Every GPU thread's stack receives full 272-byte copy via kernel-argument buffer path. On RDNA/GCN adds measurable argument-passing overhead.
- Structs this large risk hitting HIP/AMDDriver kernel-argument limit (varies per target; typically 1024–4096 bytes total across all args).

**Recommended fix**: replace `AdmBufferHip buf` parameters with
`const AdmBufferHip * __restrict__ buf` (pass pointer to device-side
copy of struct). No correctness impact — only passing convention
changes.

Until fixed: do NOT add new `__global__` parameters of type
`AdmBufferHip` by value. Any new ADM kernel should take pointer.

## extern "C" macro-instantiation pattern is correct (Research-0755)

Several ADM kernel files (`adm_csf.hip`, `adm_csf_den.hip`,
`adm_dwt2.hip`) define `__global__` kernel bodies inside `#define`
macros, then instantiate those macros inside `extern "C" { }` block.
Correct: C++ preprocessor expands macro at point of instantiation
(inside `extern "C"`), so resulting function definition unmangled,
`hipModuleGetFunction` name lookups work. NOT an `extern "C"` gap.

Pattern is load-bearing. Do not "fix" it by adding additional
`extern "C"` declaration inside macro body — would create nested
`extern "C"` which is legal in C++ but redundant and confusing to
reviewers.

## AdmBufferHip MUST be passed by pointer — invariant (ADR-0759)

**Resolved**: P1 known issue documented above (struct-by-value in
ADM kernel signatures) fixed by ADR-0759 (PR
perf/hip-adm-buffer-by-pointer-20260529).

**Invariant going forward**: any new `__global__` kernel needing
`AdmBufferHip` (or any other large parameter struct) MUST accept it
as pointer parameter, not by value. Host launch site must:

1. Hold device-side copy of struct allocated in `init_fex_hip` (or
   equivalent init path) via `hipMalloc`.
2. Populate it via `hipMemcpy(hipMemcpyHostToDevice)` after all
   device pointers inside struct are set.
3. Pass `&dev_ptr_var` (address of device pointer variable) as
   kernel arg.

Pattern:

```c
/* host dispatch helper — correct */
AdmBufferHip *buf_dev = s->buf_dev;  /* device pointer, set in init */
void *args[] = {&buf_dev, /* ... */};
hipModuleLaunchKernel(fn, ..., args, NULL);
```

Rationale: `AdmBufferHip` ~272 bytes. Passing by value marshals full
struct through per-launch argument buffer on every call. Pointer
passing reduces this to 8 bytes (one pointer) per launch.

Same rule applies to `AdmFixedParametersHip` (~244 bytes) once that
follow-up scoped; see ADR-0759 alternatives table. Do not add new
by-value large struct parameters to ADM kernels without explicit ADR
justification.

## ms_ssim_vert_lcs kernel and host partials must both be `double` (ADR-1071)

`ms_ssim_score.hip` kernel's `ms_ssim_vert_lcs` function and host
extractor `integer_ms_ssim_hip.c` share pair of allocation /
DtoH-copy contracts that are **rebase-sensitive**: one side updated
without other -> allocation sizes mismatch, HIP runtime silently
writes `float` values into `double`-sized buffer (or vice versa),
producing numerical garbage.

**Invariant:**

1. Kernel (`ms_ssim_vert_lcs`) writes `double *l_partials`, `double *c_partials`,
   `double *s_partials` — one `double` per HIP block.
2. Host (`MsSsimStateHip`) allocates device and pinned-host partial arrays as
   `sizeof(double)` per block slot (across all 5 MS-SSIM scales).
3. DtoH copy: `hipMemcpyAsync(..., sizeof(double) * num_blocks, ...)`.
4. Host accumulator: `double *h_{l,c,s}_partials[MS_SSIM_SCALES]`.
5. `c1`, `c2`, `c3` = `double` in both kernel params and `MsSsimStateHip`.

Established by ADR-1071 as direct port of CUDA ADR-0990 fix. Future
refactor reverting any of these to `float` -> cross-backend parity
regresses by ~0.004 per MS-SSIM scale (failing ADR-0214 places=4
gate on AMD hardware).

**`enable_db` / `clip_db` options** also wired into HIP extractor's
`options[]` array and `collect_fex_hip()`. Do not remove them —
required for parity with CPU (`float_ms_ssim`) and CUDA
(`float_ms_ssim_cuda`) paths.

## Wiring a new HIP extractor into the build (ADR-0852 lesson)

Three files must be updated together — omitting any one silently
leaves extractor unreachable:

1. **`core/src/meson.build` `hip_kernel_sources` dict** — add
   `'<kernel_name>'` entry pointing to `.hip` source so `hipcc
   --genco` compiles HSACO blob.
2. **`core/src/hip/meson.build` `hip_sources`** — add host `.c`
   wrapper so it's compiled into HIP runtime archive.
3. **`core/src/feature/feature_extractor.c`** — add
   `extern VmafFeatureExtractor` declaration inside `#if HAVE_HIP` and
   `&vmaf_fex_*_hip` pointer in dispatch table, so
   `vmaf_get_feature_extractor_by_name` can resolve it.

Failure mode (ADR-0852): `speed_chroma_hip` and `speed_temporal_hip`
(ADR-0567) had all three implementation files committed but missing
all three wiring entries, making extractors completely unreachable
for six weeks until ADR-0852 closed gap. CI matrix had no
`enable_hipcc=true` + `name-resolve` smoke test, so omission was
invisible until manual audit.

## motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_hip` emits `motion3_v2_score` host-side in its
  flush, mirroring CPU `integer_motion_v2.c::flush` and CUDA twin
  byte-for-byte: per-frame `motion_blend(motion2, blend_factor,
  blend_offset)` then `MIN(_, motion_max_val)` clip, a `stamp_value`
  seed for `i < min_idx (= 1)`, and optional 2-tap
  `motion_moving_average`, via shared `motion_blend_tools.h` helper.
  Any change to CPU flush blend/clip/seed/average logic must be
  mirrored into all four GPU twins (cuda/sycl/hip/metal) in same PR
  to keep `places=4` `test_hip_motion_v2_parity` gate green.
  (`test_hip_motion_v2_parity` added in PR #913 but unregistered in
  `core/test/meson.build` until wired in
  `fix/hip-motion-v2-parity-test-wiring`.)

## Option dictionary serialization timing (ADR-1154)

Extractors providing features with parameterized names must call
`vmaf_feature_name_dict_from_provided_features` **before** assigning
internal dimension defaults (`s->w = w`, `s->h = h`) to options
marked with `VMAF_OPT_FLAG_FEATURE_PARAM`. Overwriting struct fields
with non-zero defaults before creating dictionary causes
`feature_name` to serialize dimensions as option overrides (e.g.
`_full_w_576_full_h_324`), which breaks feature lookups and parity
tests.

## Integer SSIM CPU contract (ADR-0564)

`integer_ssim_hip` publishes canonical `"ssim"` feature, carries
`VMAF_FEATURE_EXTRACTOR_HIP` -> model-driven dispatch under `--backend hip`
runs it instead of CPU `ssim`. Acceptable only while it computes what
`integer_ssim.c::calc_ssim()` computes. ADR-0564 rules out drift under this
name. Before int64 port: twin ran 11-tap float Gaussian, 4.5e-3 off CPU, had to
stay unflagged (ADR-1154).

Invariants:

- **Same kernel as CPU.** 9 integer taps `[2,9,28,55,68,55,28,9,2]`, int64
  moments, boundary *truncation*: taps outside frame skipped, weight counts
  in-bounds taps only. Do not mirror or clamp at border as VIF kernels do
  (ADR-1103); CPU does neither here.
- **Same per-pixel expression.** `issim_pixel_term()` =
  `ssim_reduce_row_range()` operand for operand. `SSIM_K1` / `SSIM_K2`
  spelled `(0.01 * 0.01)` / `(0.03 * 0.03)`; literals `0.0001` / `0.0009` are
  different doubles. `hip_cu_extra_flags` builds kernel with
  `-ffp-contract=off` -> nothing fuses into FMA. With both, 1x1 frame matches
  CPU exactly. Only remaining difference: summation order, 2e-14 on Netflix
  pair, 1.06e-11 worst case (1080p checkerboard), same as CUDA twin.
- **Wavefront-independent reduction.** Per-block sums use shared-memory tree
  over all 128 threads, not `warpSize` shuffles -> wave32 (RDNA) and wave64
  (GCN / CDNA) add in same order. Tree sized for 16x8 launch:
  `ISSIM_BLOCK_X/Y` in kernel and `ISSIM_HIP_BLOCK_X/Y` in host change
  together.
- **Wait for picture upload.** `submit()` stages host pictures through
  `vmaf_hip_picture_upload()`; see "Picture uploads" below. Race first seen in
  this twin: off by up to 0.2 on Netflix 576x324 pair.

## Picture uploads: never return from submit() with one in flight

**Invariant: extractor must not return from `submit()` while upload from
pooled host picture is in flight.**

HIP pictures = pageable host memory; no HIP picture pool yet (T7-10c).
`hipMemcpy2DAsync` is asynchronous with respect to host: bare call can still
read `VmafPicture::data` after `submit()` returned, and caller may refill
picture at once. CLI pool is LIFO: distorted picture of frame N = first buffer
refilled for frame N + 1. Result: frames scored against next frame's samples.
Different set on every run. With several extractors in one process: same wrong
46 of 48 frames on every run (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).

Rules:

- Stage every plane from `VmafPicture::data` with
  `vmaf_hip_picture_upload()` (`core/src/hip/picture_hip.h`). It enqueues
  copies, waits on event recorded after them. Do not call
  `hipMemcpy2DAsync` / `hipMemcpyAsync` on picture plane directly. Copy from
  extractor-owned pinned buffer (`integer_ms_ssim_hip`,
  `integer_psnr_hvs_hip`, SpEED twins) not affected: extractor owns that
  memory until it reuses it.
- Pass extractor's private stream (`lc.str`), even when kernels run on null
  stream. Null-stream copy queues behind every null-stream kernel of frame;
  wait then blocks host on all of them. Copy completes before any kernel is
  enqueued -> no cross-stream ordering needed. About ordering guarantee, not
  speed: on gfx1036 one hardware queue serialises copy behind running kernels
  either way.
- Reference picture looks safe, is not. `vmaf_read_pictures()` keeps it alive
  one more frame through `prev_ref`; hence `motion_hip`, `motion_v2_hip`,
  `float_motion_hip` never misbehaved under CLI. libvmaf implementation
  detail, not contract: through extractor API their copy was still reading
  after `submit()` on 10 of 10 runs.
- Single-frame fixture cannot see any of this; neither can determinism check
  alone. `core/test/test_hip_upload_race.c` covers every uploading extractor
  twice: pooled frames against CPU (`hip_pooled_fixture.h`), and both pictures
  refilled the moment `submit()` returns, where every score must be
  bit-identical. Add new extractor to its `race_cases[]` table.

Wait costs host time: 21 % of `vmaf_float_v0.6.1` throughput at 1080p on
gfx1036, noise for `vmaf_v0.6.1`. Extractor-owned pinned staging buffers would
remove it; follow-up = T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19 in
`docs/state.md`. Do not buy throughput back by dropping wait.

## Integer ADM staging buffer requirement (ADR-1154, ADR-1211)

HIP pictures arrive with host pointers (host-pic backend, ADR-0530);
a host pointer handed to a device kernel faults the GPU
(T-HIP-INTEGER-ADM-GPU-PAGE-FAULT-2026-09-05). `integer_adm_hip.c`
stages the scale-0 luma plane per side in `init_fex_hip` and copies
it with `hipMemcpy2DAsync` before the DWT2 launch (ADR-1211,
PR #1370); staged rows are packed, so the kernel stride is `w`. The
ADR-1154 deferral is over: do not re-add `should_fail` to the HIP ADM
tests for it. `.flags` is still `0`, so model-driven dispatch under
`--backend hip` keeps the CPU `adm`; the twin runs when named
(`--feature adm_hip`). It has no AIM pass: `adm3_score` / `aim_score`
stay out of `provided_features[]`
(T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05). Float ADM
(`float_adm_hip.c`) has its own staging.

## float_vif options must be kernel arguments (ADR-1217)

`vif_sigma_nsq` and `vif_enhn_gain_limit` = `VMAF_OPT_FLAG_FEATURE_PARAM`
options `float_vif_hip` declares in its option table. Until
ADR-1217, compute kernel in `float_vif/float_vif_score.hip` declared
them as kernel-local constants at their default values. Non-default
value was accepted, range-checked, folded into derived feature name
(ADR-1183), then discarded. That included
`vif_enhn_gain_limit = 1.0` that `model/vmaf_float_v0.6.1neg.json` sets on all four VIF
scales, so HIP run of NEG model published ordinary
enhancement-gain-enabled scores under NEG feature keys.

Invariant: `float_vif_compute` takes `vif_sigma_nsq`, `vif_egl` and
`sigma_max_inv` as trailing kernel arguments; `fvif_launch_compute()`
derives all three from `FloatVifStateHip`. `sigma_max_inv` computed
host-side exactly as CPU computes it in
`vif_tools.c::vif_statistic_s` — `powf(nsq, 2.0f)` in `float`,
divided by `255.0 * 255.0` in `double`, narrowed to `float` — so
default path stays bit-identical to pre-ADR-1217 constant. Do not
recompute it in device code: device `powf` not guaranteed to round
like host's, and value feeds `sigma1_sq < vif_sigma_nsq` branch that
writes `num_val` directly.

`hipModuleLaunchKernel` silently ignores surplus `kernelParams`
entries, reads uninitialised memory for missing ones, so signature
and `args[]` array must change together. Guard =
`test_hip_float_vif_parity.c::test_float_vif_options_reach_kernel`,
which pins `egl=1.0 snsq=1.5`, asserts parity on derived
`vif_scale0_egl_1_snsq_1.5` key — default-options test cannot see
this class of defect, because hardcoded values *were* defaults.

## SpEED singular-covariance contract (ADR-1202, ADR-1218)

25x25 SpEED covariance matrix regular only if **every** eigenvalue
at least `1e-6`. CPU treats singular one as routine numerical
condition, not failure; `speed_chroma_hip.c` / `speed_temporal_hip.c`
must match it on two counts.

1. **Zero DEVICE solution.** Score kernel reads `d_sol`;
   `h_indterm` re-downloaded from `d_indterm` at top of every
   pipeline run. `memset`-ing host buffer = dead code, leaves
   `d_sol` holding previous frame's solution — or raw allocator
   memory on first frame. Use
   `hipMemsetAsync(d_sol, 0, indterm_bytes, s->stream)`.
2. **Report singularity out-of-band.** `run_cpu_linalg_sc()` and
   `run_cpu_linalg_st()` take `bool *singular_out`; `int` return
   stays reserved for hard HIP failures. Conflating the two makes
   caller abort channel without emitting score, where CPU emits
   one — regression ADR-1202 had to undo. Caller then applies CPU
   rule from `speed_extract_score()`: score `0` when exactly one of
   ref/dis was singular; for chroma, impute `speed_chroma_uv` from
   surviving channel.

Guarded by `core/test/test_hip_speed_singular_parity.c`. Older
`test_hip_speed_{chroma,temporal}_parity.c` fixtures are 768x432,
whose chroma planes give 4x2 = 8 blocks for 25x25 covariance —
singular on every frame — never exercise regular path. SpEED test
needing regular frame must be at least 960x960.

## CAMBI: use the shared TVI helper and the CPU's border rules (ADR-1219)

Three exact-logic traps, all of which HIP twin fell into, together
collapsed its CAMBI score to **exactly 0.0** on banding content CPU
scores at 5.85.

1. **Call `vmaf_cambi_init_tvi_and_vlt()`; never re-derive TVI
   table.** Runs CPU's own bisection of
   `tvi_hard_threshold_condition` between `luma_range.foot` and
   `luma_range.head - diff - 1`, plus `vlt_luma` and derived-band
   validation. Two independent hand-ports (HIP and Metal) both
   searched *negated* predicate seeded from luma 0, giving
   `tvi_for_diff = [1026, 1025, 1024, 4]` against CPU's
   `[182, 309, 436, 563]`. Host-side scalar work done once in `init()`, so
   per-backend copy buys nothing.

2. **`cambi.c::filter_mode` leaves output rows 0 and `height-1`
   UNFILTERED.** Vertical writeback under `if (i > 1)`, covers rows
   `1 .. height-2`; horizontal results for border rows live only in
   3-row ring, never written back. Kernel guard =
   `if (axis == 1 && (y == 0 || y >= height - 1)) return;` — V pass writes into buffer
   that still holds pre-filter image, so returning early preserves
   original pixels exactly.

3. **`get_spatial_mask_for_index()` ZERO-PADS its 7x7 box sum.**
   Summed-area table `memset` to zero, gated by
   `deriv_valid = (i < height)`, so out-of-frame tap adds nothing. Clamping taps to
   border pixel counts zero-derivative flag up to three extra times
   per axis, flips `box_sum > mask_index` on band of border pixels.

CAMBI parity fixture must band: CAMBI counts neighbour differences
of `1 .. num_diffs` (4 at default), so 8-bit gradient stepping 32
levels every 32 columns scores 0.0 on CPU too, makes assertion
`0 == 0`. Use 10-bit gradient of one level every two columns inside TVI
band (200..900), assert CPU score is non-degenerate first.

## float_adm options must reach the kernels (ADR-1220)

`adm_p_norm` (alias `apn`) = `VMAF_OPT_FLAG_FEATURE_PARAM` that
`float_adm_hip` declares with CPU's name, alias, default and range.
Until ADR-1220, `float_adm/float_adm_score.hip` hardcoded cube sum
and `float_adm_hip.c` hardcoded `1.0f / 3.0f` pooling root, so option
moved only AIM exponent, produced hybrid quantity.

Invariants:

- `adm_p_norm` has **four** application points in `adm_tools.c` —
  DLM numerator sum, CSF denominator sum, pooling root
  `powf(accum, 1.0f / adm_p_norm)`, and
  `get_noise_constant(w, h, weight, p)`. Change them together.
- Keep CPU's `p == 3` literal-cube fast path in kernel
  (`fadm_pnorm_term`). Device `powf(x, 3.0f)` not guaranteed to
  equal `x * x * x`; default path is what every shipped model uses.
- `hipModuleLaunchKernel` silently ignores surplus `kernelParams`,
  reads uninitialised memory for missing ones, so kernel signature
  and `args[]` arrays for **both** `func_csf_cm` and `func_aim_cm`
  change together.

This twin does not declare `adm_bypass_cm`, rejects it; deliberate,
adding it tracked in `docs/state.md`. Guarded by
`test_hip_float_adm_parity.c::test_float_adm_p_norm_reaches_kernel`.

## MS-SSIM clip_db is a dB ceiling (ADR-1221)

`float_ms_ssim.c` derives
`max_db = ceil(10 * log10(peak * peak / mse))` with
`mse = 0.5 / (w * h)` at `init()`; `convert_to_db()`
returns `MIN(-10*log10(1 - score), max_db)`, short-circuiting to
`max_db` when `score >= 1.0`. Until ADR-1221, `integer_ms_ssim_hip.c`
clamped LINEAR score into `[0, 1]`, converted with no ceiling, had no
`max_db` field: identical reference/distorted pair returned `+Inf`;
every high-similarity pair returned uncapped dB value.

`max_db` derived once in `init_fex_hip` right after
`ms_ssim_hip_init_dims()`, using CPU's exact expression and integer
types; dB conversion goes through `ms_ssim_convert_to_db()`. Guard =
`test_hip_ms_ssim_parity.c::test_ms_ssim_clip_db_ceiling`, which
feeds IDENTICAL pair — on merely high-similarity fixture, ceiling
never binds, variant passes against unfixed twin.

## Scaffold posture returns -ENOSYS, nothing else (ADR-1264)

`enable_hipcc` defaults **false** -> no device kernels -> extractor must report
`-ENOSYS`. Parity tests turn that into `[skip: ...]` and pass.

Scaffold path returns `-ENOSYS` **directly**. Never call a kernel-submit helper
with placeholder args first: `vmaf_hip_kernel_submit_pre_launch(..., NULL, ...)`
rejects NULL `rb` as its first statement, so it always returns `-EINVAL` and any
`return -ENOSYS` after it is dead code. That shape failed
`test_hip_float_vif_parity` and `test_hip_psnr_hvs_parity*` on every default
build.

Writing a new HIP parity test: check `-ENOSYS` at **both** sites --
`vmaf_use_feature()` AND `vmaf_read_pictures()`. Extractor may give up at
registration or inside `extract()`; `speed_temporal_hip` does the latter.
Reference shape: `core/test/test_hip_speed_temporal_parity.c`.

## `__HIP_PLATFORM_AMD__` comes from the build (ADR-1263)

New HIP host source needs **no** `#define __HIP_PLATFORM_AMD__`. `hip_deps` in
`core/src/hip/meson.build` supplies `-D__HIP_PLATFORM_AMD__=1` to every HIP TU,
outside the `hip_runtime_dep` discovery branch, so both branches get it.

Copying the old `#define` from a neighbour re-adds a reserved identifier
(`cert-dcl37-c`): next PR touching that file then owns removing it.

## No path globs in block comments

`core/src/feature/hip/*.c` inside `/* ... */` opens nested comment ->
`-Wcomment` on every HIP build -> zero-warning gate fails. 14 parity tests had
it. Name the set in prose: "the .c files under core/src/feature/hip/".
`max_db` is derived once in `init_fex_hip` right after
`ms_ssim_hip_init_dims()`, using the CPU's exact expression and integer
types, and the dB conversion goes through `ms_ssim_convert_to_db()`.
The guard is `test_hip_ms_ssim_parity.c::test_ms_ssim_clip_db_ceiling`,
which feeds an IDENTICAL pair — on a merely high-similarity fixture the
ceiling never binds and the variant passes against the unfixed twin.

## Integer ADM tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_hip()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- Host shift rounding constant = `adm_half_shift(x)`. In-kernel scale-0 shift
  in `adm_cm_reduce_line_kernel_body` -> guarded ternary, 0 when shift = 0.
  Never bare `1u << (x - 1)`.
- Scale-0 CM kernel (`adm_cm_line_kernel_body`): `x + 1` -> `min(.., w - 1)`,
  `y + 1` -> `min(.., h - 1)`; `x - 1`, `y - 1` -> `abs()` (ADR-1210 rule).
- HIP twin emits no `adm3_score` (T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05).
  Shared CUDA/HIP tests skip `adm3` under `HAVE_HIP`.
- HIP ADM tests run without `should_fail` since ADR-1211 staging; all pass on
  gfx1036. Do not re-add `should_fail` to hide a failure.
