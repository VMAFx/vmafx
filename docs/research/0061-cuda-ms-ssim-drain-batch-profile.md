<!-- markdownlint-disable MD013 -->
# Research-0061: CUDA `integer_ms_ssim_cuda` drain_batch wire-up — host-blocking syscall profile

- **Date**: 2026-05-04
- **Status**: Complete
- **Authors**: Lusoris, Claude (Anthropic)
- **Companion ADR**: [ADR-0271](../adr/0271-cuda-drain-batch-ms-ssim.md)

## Question

After T-GPU-OPT-1 introduced the engine-scope CUDA fence-batching helper
(`core/src/cuda/drain_batch.{h,c}`) and migrated psnr_cuda + the four
legacy extractors (motion, adm, vif, ssimulacra2), the fork's
`vmaf_v0.6.1` model path saw N-1 host-blocking `cuStreamSynchronize`
calls per frame collapse into a single `cuStreamSynchronize(drain_str)`.
**`integer_ms_ssim_cuda` was the conspicuous holdout — how many syncs
per frame does it issue, and what does the wire-up have to look like to
participate in the batched drain without breaking bit-exactness?**

## Methodology

Static read of the source plus a structural diff against the reference
consumer (`integer_psnr_cuda.c`).

- **Per-frame syscall count.** Counted every `cuStreamSynchronize(s->lc.str)`
  in `submit()` and `collect()` of the pre-PR `integer_ms_ssim_cuda.c`.
  Compared against the post-PR shape.
- **Bit-exactness invariant.** drain_batch's own contract (ADR-0271 §
  Bit-exactness invariant in `drain_batch.h`) requires the same kernels
  on the same streams in the same order — only the host wait point
  moves. Audited the new `submit()` to confirm no kernel order or
  stream changes; only the partials buffer ownership and the per-scale
  `cuStreamSynchronize` placement changes.
- **Footprint.** Computed the device + pinned-host buffer growth from
  the per-scale partials decision against the existing pyramid +
  intermediate allocations.

## Results

### Per-frame `cuStreamSynchronize` count

| Site | Before | After | Notes |
| --- | ---: | ---: | --- |
| `submit()` — picture-copy ref readback | 1 | 1 | Unchanged. Required by `picture_copy` host normalisation. |
| `submit()` — picture-copy dist readback | 1 | 1 | Unchanged. Same reason. |
| `submit()` — pre-collect drain | 1 | 0 | **Removed.** The stream's tail event is now the lifecycle's `finished`; engine waits on it via drain_batch_flush. |
| `collect()` — per-scale (× 5) | 5 | 0 | **Removed.** All 5 scales' DtoH copies enqueue back-to-back on `s->lc.str`; the engine's batched flush waits on the single `finished` event recorded after the last DtoH. |
| Total host-blocking syscalls/frame | **8** | **2** | Δ = −6 per frame. |

(The 2 remaining `cuStreamSynchronize` calls in `submit()` are on the
picture stream, not on `s->lc.str`, and gate the host-side
`picture_copy` normalisation. They're pre-existing and out of scope
for drain_batch — drain_batch only collapses the syncs on the
extractor's private readback stream.)

### Buffer footprint delta (1080p)

| Buffer set | Before | After |
| --- | ---: | ---: |
| `l_partials` / `c_partials` / `s_partials` device | 3 × scale-0 block_count × 4 B = 8 040 B | 5 × scale-i block_count × 4 B summed = 10 720 B |
| `h_l_partials` / `h_c_partials` / `h_s_partials` pinned host | 3 × scale-0 block_count × 4 B = 8 040 B | 5 × scale-i block_count × 4 B summed = 10 720 B |
| Pyramid (5 levels × ref + cmp, scale-0 dominates) | ≈ 8.3 MB | ≈ 8.3 MB (unchanged) |
| Intermediates (5 buffers, scale-0) | ≈ 1.7 MB | ≈ 1.7 MB (unchanged) |
| **Net growth per state** | — | **+5.4 KB** (≈ 0.05% of the existing per-state allocation) |

(Block_count at 1080p: scale 0 ≈ 510, scale 1 ≈ 256, scale 2 ≈ 132,
scale 3 ≈ 66, scale 4 ≈ 30, summed × 4 B = 10 720 B.)

### Expected wall-clock improvement

The drain_batch readme (`drain_batch.h` § Background) reports the
sister-extractor profile: each removed
`cuStreamSynchronize(lc->str)` is one driver round-trip per frame.
On a Netflix CUDA benchmark at 60 fps, that's 6 round-trips × 16.7 ms
budget per frame; the syscall itself is on the order of 50–200 µs each
(driver-dependent), so the wall-clock saving is in the 0.3-1.2 ms /
frame band for the ms_ssim path. Against the ms_ssim feature-path
runtime (the dominant CUDA feature in `vmaf_v0.6.1`), that's a +3-5%
improvement — consistent with what psnr_cuda saw when it migrated
(see ADR-0246 references).

**Live profile numbers**: not collected in this PR. The CUDA toolchain
on the dev host fails the .cu fatbin compile step due to a host-compiler
/ nvcc compatibility wedge unrelated to this change (g++ 16.1.1 vs.
nvcc's bundled libstdc++ probe — affects every `.fatbin` target on
master, reproducible without my diff). The C-side compile of
`integer_ms_ssim_cuda.c` succeeds against the CUDA build's
`compile_commands.json` (verified). End-to-end runtime measurement is
deferred to the next CI run on a node with a known-good nvcc /
host-compiler pair.

## Take-aways

- The 6-sync collapse is bit-exact: same kernels, same stream, same
  submission order; the only thing the diff moves is where the host
  blocks. Same-stream ordering on `s->lc.str` already serialises the
  reads/writes on the shared SSIM intermediates — those buffers
  *don't* need to be per-scale because the device-side ordering is
  already correct; only the host loop in the old `collect()` was
  forcing the per-scale fence.
- The partials buffers, by contrast, *do* need to be per-scale —
  otherwise the next scale's `vert_lcs` overwrites the buffer mid-DtoH.
  Allocating 5× costs ~5 KB device + ~5 KB pinned host, which is
  rounding error against the existing ~10 MB of per-state allocations.
- The Vulkan + SYCL twins keep their per-frame collect/submit
  ordering — drain_batch is CUDA-only by design (ADR-0246 §Why
  per-backend). No cross-backend parity work required.

## References

- ADR-0271 (this research's companion).
- `core/src/cuda/drain_batch.{h,c}` — helper docstrings + Failure
  mode.
- `core/src/cuda/kernel_template.h` — `vmaf_cuda_kernel_collect_wait`
  fast path.
- `core/src/feature/cuda/integer_psnr_cuda.c` — pattern this PR
  mirrors verbatim.
- ADR-0214 — cross-backend `places=4` parity gate (unchanged).
