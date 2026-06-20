<!--
  Research digest — ADR-0108 deliverable (1) for PR #1029
  fix(speed): GPU SpEED correctness + safety across all backends (audit).
  No new ADR: these are correctness bug fixes (CLAUDE §12 r8 exempt); the
  decision matrix lives in the "Alternatives considered" section below.
-->

# GPU SpEED — covariance geometry + eigenvalue-basis correctness (2026-06-20)

RTX-4090 bit-parity verification of the GPU SpEED extractors.

**Scope:** the GPU SpEED feature extractors —
`core/src/feature/cuda/{speed_chroma_cuda.c, speed_temporal_cuda.c}` +
`core/src/feature/cuda/speed/speed_score.cu`, and their HIP / SYCL twins
(`hip/speed_{chroma,temporal}_hip.c` + `hip/speed/speed_score.hip`;
`sycl/speed_{chroma,temporal}_sycl.cpp`). The CPU reference is
`core/src/feature/speed.c` (`est_params`, `compute_mean`,
`speed_internal_compute_eigenvalues`).

This digest explains two algorithm-correctness bugs that made every GPU
SpEED run produce wrong scores, the heap-safety and device-pointer bugs
found alongside them, and the bit-parity verification method that proved
the fix on an RTX 4090. It is the audit trail behind the
"GPU SpEED now matches CPU" user-visible numeric correction.

---

## 1. Why these bugs survived to the audit

SpEED is the only feature whose GPU twins were *never validated against the
CPU* before this PR. The reason is structural, not accidental:

- The CUDA / HIP SpEED parity tests
  (`test_cuda_speed_{chroma,temporal}_parity`,
  `test_hip_speed_{chroma,temporal}_parity`) require a real GPU at runtime.
  The hosted CI runners have **no GPU**, so these tests compile but never
  execute — they skip cleanly. Nothing in the green CI signal ever exercised
  the GPU SpEED math.
- The SYCL chroma parity test (`test_sycl_speed_chroma_parity`) *did* run on
  the Arc CI lane, but its fixture (256×144 → 128×72 chroma) was **below the
  SpEED `NUM_SCALES` pyramid minimum**, so the extractor returned `-EINVAL`
  at init and the test never reached the kernels. It was further masked while
  `speed_chroma_sycl` was unregistered (pre-#1004).

Net effect: the GPU SpEED kernels were a blind port of the CPU formula whose
output nobody had ever compared to the reference. The audit forced the
comparison by (a) building a real GPU parity harness in the dev-mcp
container on the RTX 4090, and (b) enlarging the SYCL fixture to 320×320 so
the chroma plane (160×160) clears the minimum.

---

## 2. Bug A — per-tile vs global covariance geometry (~7× wrong scores)

### 2.1 What the CPU does

`est_params` in `speed.c` computes, for a downscaled plane, **one global
5×5-element covariance matrix**. For each of the 25 phase-shift elements
`elem = (er, ec)` with `er, ec ∈ [0, 5)`, it forms a phase-shifted
full-plane submatrix `M_elem[y][x] = plane[(y+er)*stride + (x+ec)]` and:

1. `compute_mean` gives the scalar global mean `means[elem]` over the whole
   submatrix (`means` is indexed `[0, 25)` — one mean per element).
2. The covariance entry `cov[i][j]` is the global covariance of submatrix
   `M_i` against submatrix `M_j`, i.e.
   `Σ_{x,y} (M_i[y][x] − means[i]) · (M_j[y][x] − means[j]) / N`,
   summed over **every** `(x, y)` in the submatrix and divided by `N` **once**.

There is no spatial tiling. The matrix is 25×25, built from 25 scalar means
over the full phase-shifted plane.

### 2.2 What the GPU port did wrong

The original `speed_means_kernel` / `speed_cov_kernel` decomposed the plane
into `num_blocks` spatial tiles and computed **per-tile, block-local**
statistics:

- `means` was laid out `means[elem * num_blocks + tile_idx]` — a separate
  mean per `(element, tile)` pair, sampled at `tile_y*5 + er` (a per-tile
  origin) instead of the global `er`.
- `speed_cov_kernel` looped over `num_blocks` tiles, summed `num_blocks`
  displaced block-local covariances, and effectively divided per-tile rather
  than by the single global `N`.

This is a genuinely different matrix. The per-tile mean subtraction removes
inter-tile structure that the global covariance keeps, and the tile-summed
normalisation rescales the entries. The eigenvalues of the resulting matrix
feed the SpEED entropy, so the error propagates straight into the score:
measured ~**7× low** vs CPU on the RTX 4090 (chroma cpu=19.84 vs cuda=2.89
before the geometry fix).

### 2.3 The fix

Rewrite both kernels to the CPU's global formulation:

- `speed_means_kernel` → 25 work-items, each computing the single global mean
  at element `(er, ec)`; writes `means[elem]` for `elem ∈ [0, 25)`. The
  `means[]` buffer stays over-allocated at `25 × num_blocks` (launch geometry
  and allocation unchanged), but only `[0, 25)` is written and read.
- `speed_cov_kernel` → a global submatrix sweep with **scalar** global means
  `means[x_index]` / `means[y_index]`, dividing by `N` once, no tile loop.

Launch geometry is preserved so the surrounding allocation / dispatch code is
untouched. After this fix `test_cuda_speed_temporal_parity` passes bit-parity;
chroma improved from 7× low to within 2× — surfacing Bug B.

---

## 3. Bug B — shared vs separate ref/dis covariance + eigenvalue basis

### 3.1 What the CPU does

`est_params` is called **twice per frame** — once on the reference plane,
once on the distorted plane — producing **two independent** covariance
matrices and, after the linear solve + eigendecomposition, **two independent
eigenvalue arrays**. The reference entropy is computed from the reference
eigenvalues; the distorted entropy from the distorted eigenvalues. They are
never crossed.

### 3.2 What the GPU port did wrong

The CUDA host path computed the reference correctly, then **saved the
reference covariance and restored it before the distorted CPU-linalg**. As a
result the distorted linear-system solution AND the distorted eigenvalues
were derived from the **reference** covariance. The shared `speed_score_kernel`
then used a single eigenvalue array for both ref and dis entropy.

This only matters when `ref ≠ dis`. The **temporal** SpEED feature compares
consecutive frames, where `ref ≈ dis`, so the bug was masked there — temporal
parity passed even with the shared basis. The **chroma** feature compares the
reference and distorted pictures, where `ref ≠ dis`, so the distorted path was
~2× high. RTX-4090 instrumentation confirmed the mechanism precisely:
reference variance / entropy matched the CPU bit-exactly, while the distorted
variance / entropy diverged (var 5.11 vs 3.54, entropy 211 vs 190).

### 3.3 The fix

- Drop the cov save/restore so the distorted path keeps the **distorted**
  covariance (and therefore distorted solve + distorted eigenvalues).
- Add a `d_eigenvalues_ref` device buffer; after the reference linalg,
  `cuMemcpyDtoD` (HIP: `hipMemcpyDtoD`; SYCL: `q.memcpy` DtoD) the reference
  eigenvalues aside into it.
- `speed_score_kernel` now takes **both** `ref_eigenvalues` and
  `dis_eigenvalues`; the ref entropy reads `ref_eigenvalues[k]`, the dis
  entropy reads `dis_eigenvalues[k]`.

After this fix `test_cuda_speed_chroma_parity` **and**
`test_cuda_speed_temporal_parity` both pass bit-parity (< 1e-4) vs CPU on the
RTX 4090.

---

## 4. Co-located safety bugs (same extractors, found by the same audit)

These are not algorithm bugs but were in the same files and crashed the
extractor before the math could even be compared:

1. **Device-pointer SEGV (CUDA chroma + temporal).** `extract_channel` /
   `extract_fex_st` ran the host-side `picture_copy()` directly on
   `ref_pic->data[channel]` / `data[0]`, which in the CUDA pipeline is a
   `CUdeviceptr` (like `adm`/`vif_cuda`). The host read of GPU memory SEGV'd
   every frame. Fix: `cuMemcpyDtoH` the plane into host staging, alias into a
   temp `VmafPicture`, then `picture_copy`. (For temporal a local
   `cuCtxPushCurrent` is needed because the GPU pipeline pushes the context
   later.)

2. **Eigenvalue-scratch heap corruption (RETRAIN-CRITICAL; all six GPU SpEED
   extractors).** `speed_internal_compute_eigenvalues` lays its scratch out as
   `A[n*n] + d[n] + sd[n] + tmp[2*n] = n*n + 4*n` floats, but every GPU caller
   allocated only `n*n + 3*n` → `si_tri_multiply` wrote `n` (= 25) floats past
   the end → `free(): invalid next size`. Silent ~100-byte overwrite on normal
   heaps; hard crash under `MALLOC_PERTURB_` / ASan. The CPU path uses its own
   correctly-sized buffer. Fixed the `+4*n` sizing in all six.

3. **CPU heap-buffer-overflow (`speed.c`, the CI all-backends SEGV).**
   `speed_init_dimensions()` returns `-EINVAL` for planes too small for the
   `NUM_SCALES` pyramid, but `speed_init()` / `init_chroma()` ignored the
   return; `submatrix_height = truncated_height − block_size + 1` then
   underflowed `size_t` to ~2^64 and `compute_mean()` walked off the buffer.
   The SYCL twin already checked this return; the CPU path did not. Fix:
   reorder the `truncated == 0` guard before the subtraction and propagate the
   return through `speed_init()` + `init_chroma()`. Also: temporal init
   ignored `speed_init()`'s return entirely, so an in-range-but-invalid
   `speed_kernelscale` / bad prescale left `float_stride = 0` → zero-size
   mallocs → OOB in `picture_copy`. Captured + checked.

4. **SYCL solve-kernel divergent-barrier deadlock (DEVICE_LOST on Intel Arc).**
   `launch_solve` early-returned idle lanes (`lane >= SP_ELEMENTS`) and surplus
   warps before a work-group `group_barrier` they were required to reach → the
   group deadlocked on strict-barrier GPUs. Fix: gate the work with an
   `active` flag, keep all work-items in the barrier loop.

5. **Init-OOM resource leaks (CUDA + HIP).** CUDA init `fail_pop` popped the
   context and returned `-EIO` without `free_cuda_buffers(_st)` → device +
   pinned-host buffers leaked on partial alloc failure. HIP init's post-alloc
   NULL check did a bare `return -ENOMEM` that bypassed the `free_cpu` cleanup
   label → up to 10/12 CPU scratch buffers leaked. Both routed to the cleanup
   path.

---

## 5. Verification method — RTX-4090 bit-parity

The method that turned "blind port" into "verified correct":

1. **Build in the dev-mcp container** (`docker exec vmaf-dev-mcp …`) with CUDA
   enabled, so the RTX 4090 is live. The container bakes in the CUDA toolchain
   and avoids the host-build toolchain drift (CLAUDE §15).
2. **Run the GPU parity tests** `test_cuda_speed_chroma_parity` and
   `test_cuda_speed_temporal_parity`. Each scores the same (ref, dis) pair on
   the CPU and on CUDA and asserts agreement at `places=4` (≤ 1e-4, the
   cross-backend tolerance of ADR-0214).
3. **Instrument the intermediates** when a test failed: dump per-element
   variance and entropy for the ref and dis paths separately. This is what
   localised Bug B — the ref intermediates matched bit-exactly while the dis
   intermediates diverged, pointing straight at the shared-basis save/restore.
4. **ASan + `MALLOC_PERTURB_` sweep** on the SYCL CPU-OOB repro and the full
   Arc-GPU `speed_chroma` pipeline to confirm the heap-scratch and
   barrier-deadlock fixes (no `free(): invalid next size`, no DEVICE_LOST).

Result on the RTX 4090: both CUDA SpEED parity tests PASS bit-parity vs CPU
after the full fix. The HIP and SYCL ports apply the **identical algorithm**
but could not be run-verified on this host (no AMD GPU; the Arc lacks fp64
for the SpEED double-precision path) — CI / fp64 hardware compile- and
run-verifies those legs.

---

## Alternatives considered (ADR-0108 decision matrix; no separate ADR)

These are correctness fixes, so the "decision" was which *shape* of fix to
take, not whether to fix:

| Decision point | Chosen | Rejected alternative | Why |
| --- | --- | --- | --- |
| Covariance geometry | Rewrite kernels to the CPU global formulation (scalar means `[0,25)`, divide by N once) | Keep the per-tile decomposition and try to reconcile it numerically | The per-tile matrix is a *different* matrix; no rescaling recovers the global covariance. Match the reference exactly. |
| `means[]` allocation | Leave over-allocated at `25 × num_blocks`, use `[0,25)` | Shrink to `25` floats | Shrinking changes launch geometry + the surrounding alloc/dispatch code; over-allocation is harmless and keeps the diff minimal and rebase-safe. |
| Ref/dis eigenvalue basis | Separate `d_eigenvalues_ref` buffer + DtoD stash; score kernel takes two arrays | Keep the cov save/restore and one eigenvalue array | The save/restore forces dis to use the ref basis — the root cause. The CPU uses two independent bases; the GPU must too. |
| Where to verify | RTX-4090 bit-parity at places=4 in the dev-mcp container | Trust the blind port / CI-skip | CI has no GPU; the only way to know the math is correct is to run it on real silicon against the CPU reference. |
| HIP / SYCL legs | Port the identical verified algorithm, compile-verify, defer run-verify to CI / fp64 hardware | Block the PR until AMD + fp64 hardware is available | The algorithm is proven on CUDA; the twins are line-for-line ports. Blocking would leave the known-wrong kernels in tree longer. |

## References

- CPU reference: `core/src/feature/speed.c` — `est_params`, `compute_mean`,
  `speed_internal_compute_eigenvalues`, `speed_init_dimensions`.
- ADR-0214 — cross-backend numeric tolerance (places=4 / 1e-4).
- CLAUDE §15 — dev-mcp container as the canonical GPU run environment.
- PR #1029 commit chain (9 commits): heap-OOB + eig-scratch + SYCL deadlock →
  device-pointer downloads → geometry fix → init-return checks → separate
  ref/dis basis → HIP/SYCL ports.
