# ADR-0410: `ssimulacra2_cuda` GPU module leak + per-scale `malloc` removal

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude
- **Tags**: cuda, gpu, perf, memory-leak, ssimulacra2, fork-local

## Context

A 2026-05-09 cuda-reviewer pass over
[`core/src/feature/cuda/ssimulacra2_cuda.c`](../../core/src/feature/cuda/ssimulacra2_cuda.c)
(originally landed in PR #162 / [ADR-0206](0206-ssimulacra2-cuda-sycl.md))
surfaced four issues in the `extract` and `close` paths:

1. **GPU module leak** — `init_fex_cuda` calls `cuModuleLoadData`
   twice (the `ssimulacra2_blur` and `ssimulacra2_mul` fatbins) and
   stores the module handles in `Ssimu2StateCuda`. `close_fex_cuda`
   destroys the stream and frees the buffer pool but never calls
   `cuModuleUnload` on either handle. Each module carries
   ~200-500 KB of GPU-resident backing store that is not reclaimed
   by `cuStreamDestroy` and (for the primary context the picture-pool
   uses) survives `cuCtxDestroy` as well. Repeated init/extract/close
   cycles (long-running ffmpeg pipelines that re-initialise libvmaf
   per shot, batch CI runs, the MCP server's stateless-call mode)
   accumulate hundreds of MB of GPU memory before the host process
   exits. `compute-sanitizer --tool memcheck` does **not** flag the
   leak — the tool's leak-checker tracks `cuMem*Alloc` only and is
   blind to module backing storage.

2. **Per-scale `malloc` in the hot path** — the per-scale
   downsample loop in `extract_fex_cuda` allocated a
   `3 * width * height * sizeof(float)` scratch buffer per scale per
   frame (24 MB at 1080p × up to 5 scales / frame). On a warm glibc
   allocator this is cheap, but a memory-pressured host falls back
   to `mmap`/`brk` and pays the syscall + first-page-fault cost on
   every scale, every frame.

3. **Full-resolution H2D / D2H every scale** — the upload of
   `d_ref_xyb` / `d_dis_xyb` and the download of the 5 blurred
   buffers (`mu1` / `mu2` / `s11` / `s22` / `s12`) used the full
   `3 * width * height * sizeof(float)` byte count even when only
   the leading `scale_w * scale_h` pixels of each plane carried valid
   data. At scale 2 of 1080p this is 518 KB of valid data per plane
   versus an 8 MB transfer per copy — a ~15× PCIe traffic excess
   per copy, repeated 5× per scale.

4. **No `__launch_bounds__` on the blur kernels** — `nvcc` had no
   documented occupancy contract for `ssimulacra2_blur_h` /
   `ssimulacra2_blur_v` and chose a register budget that left
   resident-block count below the 32-block target the host launch
   shape (`SS2C_BLUR_BLOCK = 64`) implies.

The architectural ceilings on the H-pass non-coalesced reads and the
V-pass L1 pressure (warnings 3 + 4 in the review) require a shared-
memory tile-transpose rewrite and are documented as known follow-ups.

## Decision

We will:

1. Add guarded `cuModuleUnload` calls in `close_fex_cuda` for
   `s->module_blur` and `s->module_mul`, after `cuStreamSynchronize`
   and before `cuStreamDestroy`.
2. Pre-allocate two pinned scratch buffers (`h_ref_lin_ds`,
   `h_dis_lin_ds`) once in `ss2c_alloc_buffers` via
   `vmaf_cuda_buffer_host_alloc` and reuse them across scales,
   removing the per-scale `malloc` / `free` pair.
3. Replace the single full-plane `cuMemcpyHtoDAsync` /
   `cuMemcpyDtoHAsync` with three per-plane copies of
   `scale_w * scale_h * sizeof(float)` each, offset by
   `plane_full_pixels * sizeof(float)` so the device-side stride
   contract (kernels assume planes at `plane_full_pixels` offsets,
   immutable across scales) is preserved.
4. Annotate `ssimulacra2_blur_h` and `ssimulacra2_blur_v` with
   `__launch_bounds__(64, 32)` so `nvcc` trims registers to keep
   ≥32 resident blocks per SM.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Leave the module leak, document it | Zero code change. | Linear GPU memory growth across init/extract/close cycles; long-running pipelines OOM eventually. `compute-sanitizer` blind-spot makes the leak invisible to the standard fork tooling. | The fix is two guarded calls and matches the runtime contract every other CUDA extractor *should* honour (none currently do — separate sweep in scope of `T-CUDA-MODULE-UNLOAD-SWEEP` follow-up). |
| In-place 2×2 downsample (writes to first `nw*nh` pixels of each plane) | No extra pinned allocation. | The overlap region (first `nw` pixels of the source) gets clobbered before its last read, breaking the box average. Fixing requires a temporary even within a single plane. | Pre-allocated pinned scratch is cleaner and still avoids the per-frame `malloc`. |
| Pageable host scratch via `mem_alloc` (libvmaf helper) | No CUDA-pinned allocator pressure. | Host-pageable scratch defeats async DMA on the next scale's H2D — the upload would bounce through a page-locked stage anyway. | Pinned scratch is on the hot path; the existing `h_ref_lin` / `h_dis_lin` pinned reservations set the precedent. |
| Single full-buffer H2D, accept the PCIe waste | One memcpy per buffer (simpler call shape). | 15× PCIe traffic excess at scale 2 (518 KB valid vs 8 MB transferred) on a per-frame basis. | The 3-iteration loop is trivial; the device-stride contract is preserved. |
| Architectural blur rewrite (shared-memory tile-transpose) | Eliminates the H-pass non-coalesced reads and V-pass L1 pressure (warnings 3 + 4). | Multi-week effort; bit-exactness against CPU IIR pole-tracking has to be re-validated through the 6-scale pyramid. | Out of scope for this fix-PR. Documented as a known ceiling in `## Consequences` § Negative below; verification command preserved for the eventual rewrite. |

## Consequences

- **Positive**:
  - GPU module backing store is reclaimed at `vmaf_close()`. Long-
    running pipelines no longer accumulate ~500 KB / cycle of
    invisible-to-`compute-sanitizer` GPU state.
  - Per-scale 24 MB `malloc` is gone. The pinned scratch is
    allocated once at init and reused for the lifetime of the
    extractor. Memory-pressured hosts no longer pay
    `mmap`/`brk` + first-page-fault cost per scale per frame.
  - PCIe traffic at scales ≥ 2 drops by ~15× (518 KB vs 8 MB at
    1080p scale 2). At scale 5 (135×60 from 1080p) the H2D / D2H
    transfers are sub-page, well below the per-call CUDA driver
    overhead floor.
  - `__launch_bounds__(64, 32)` documents the launch contract for
    `nvcc`'s register allocator. Occupancy verification is a
    follow-up under the T-GPU-OPT umbrella.

- **Negative**:
  - Wall-clock improvement at 1080p on RTX 4090 is in noise
    (~0.7% measured across 3 trials of 48 frames). The fix is
    correctness-shaped (leak elimination, PCIe-traffic reduction)
    rather than throughput-shaped at this resolution. The host XYB
    pre-pass and the GPU IIR kernel itself dominate wall-clock; the
    fixes only affect overheads. On smaller fixtures or memory-
    pressured hosts the gain is more visible.
  - **Known ceiling — not addressed by this PR**: the H-pass
    non-coalesced reads and the V-pass L1 pressure
    (warnings 3 + 4 of the 2026-05-09 cuda-reviewer pass) require a
    shared-memory tile-transpose rewrite. Verification command for
    that follow-up:

    ```bash
    ncu --section MemoryWorkloadAnalysis \
        --kernel-id ::ssimulacra2_blur_h:1 \
        build/tools/vmaf … --feature ssimulacra2 --backend cuda
    ```

- **Neutral / follow-ups**:
  - Bit-exactness verified at `places=4` (max abs diff 0.000000e+00,
    0/48 mismatches) on the Netflix golden 576×324 pair via
    `scripts/ci/cross_backend_vif_diff.py --feature ssimulacra2 --backend cuda`.
  - The `cuModuleUnload` rule should be applied to every other
    CUDA extractor that calls `cuModuleLoadData` (sweep candidate
    `T-CUDA-MODULE-UNLOAD-SWEEP`). None of the existing extractors
    honour it; that's pre-existing fork debt outside this PR's scope.
  - `core/src/cuda/AGENTS.md` gains a `## Lifecycle invariants`
    section pinning the `cuModuleLoadData` ↔ `cuModuleUnload`
    pairing rule for future agent passes.

## References

- PR #162 — original `ssimulacra2_cuda` landing.
- [ADR-0206](0206-ssimulacra2-cuda-sycl.md) — `ssimulacra2_cuda`
  precision contract (`places=4`, `--fmad=false` on the IIR fatbin).
- [ADR-0192](0192-gpu-long-tail-batch-3.md) — GPU long-tail batch-3
  scope (parent of the `ssimulacra2_cuda` workstream).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU-parity CI gate semantics.
- Source: `req` (direct user direction, 2026-05-09 cuda-reviewer
  follow-up).
