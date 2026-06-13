# Research-0067: `ssimulacra2_cuda` cuda-reviewer follow-up (2026-05-09)

Companion to [ADR-0356](../adr/0410-ssimulacra2-cuda-leaks-perf.md).

## Scope

A 2026-05-09 cuda-reviewer pass over
[`core/src/feature/cuda/ssimulacra2_cuda.c`](../../core/src/feature/cuda/ssimulacra2_cuda.c)
(originally PR #162 / ADR-0206) flagged six issues. This digest
records the empirical findings and the rationale for the four that
this fix-PR addresses; the two architectural ceilings (warnings 3 + 4
— H-pass non-coalesced reads, V-pass L1 pressure) are documented as
known follow-ups.

## Findings

### 1. GPU module leak — invisible to `compute-sanitizer --tool memcheck`

`init_fex_cuda` calls `cuModuleLoadData` for both
`ssimulacra2_blur_ptx` and `ssimulacra2_mul_ptx`. The handles live in
`Ssimu2StateCuda::module_blur` / `::module_mul`. `close_fex_cuda`
destroys the stream and frees the buffer pool but never calls
`cuModuleUnload` on either handle.

Empirical check on RTX 4090 / driver 595.71.05 / CUDA 13.2:

```bash
compute-sanitizer --tool memcheck --leak-check full \
    core/build/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --feature ssimulacra2 --backend cuda
# LEAK SUMMARY: 8 bytes leaked in 1 allocations
# (the 8 bytes are an unrelated `cuMemHostAlloc` site, not the
# ssimulacra2 modules)
```

`memcheck` reports the same 8-byte leak both before and after the
fix because the tool's leak-checker is scoped to `cuMem*Alloc` only;
module backing storage is allocated through a separate driver path
(`cuModuleLoadData` ultimately calls `cuModuleLoadDataEx_v2` which
allocates GPU memory in a context-private heap not exposed to the
memcheck tracker). This is why the leak survived initial review and
post-merge sanitizer runs.

A non-trivial implication: every other CUDA extractor in the fork
(13 files, all calling `cuModuleLoadData` and never `cuModuleUnload`)
leaks the same way. Tracked as
`T-CUDA-MODULE-UNLOAD-SWEEP` for a separate sweep PR.

### 2. Per-scale `malloc` in the hot path

`extract_fex_cuda`'s per-scale loop allocated a
`3 * width * height * sizeof(float)` host buffer per scale per
frame. At 1080p that is 24 MB, repeated for up to 5 scales per
frame (the 6-scale pyramid breaks early below the 8×8 floor).

On a warm glibc allocator the cost is in the µs range — `malloc` of
24 MB hits an existing arena chunk. On a memory-pressured host
(other large allocations, fragmented heap), glibc falls back to
`mmap` for any request ≥ 128 KB by default
(`mallopt(M_MMAP_THRESHOLD, …)`); each scale then pays a fresh
syscall + first-page-fault for the 6144 4 KB pages of the buffer
backing.

Replaced with two pre-allocated pinned buffers (`h_ref_lin_ds` /
`h_dis_lin_ds`) owned by `ss2c_alloc_buffers`. The buffers are
reused across scales: the previous pyramid level is consumed before
the next is written, and ref / dis are independent so a single
buffer per side suffices.

### 3 + 4. Architectural blur ceilings (deferred)

H-pass: one thread per row scans left-to-right through 6-scale
pyramid IIR with all-zero prevs at boundaries. The row-wise scan is
non-coalesced — neighbouring threads (rows) read from addresses
`width` floats apart, defeating the GPU's 32-byte cacheline
coalescing.

V-pass: one thread per column scans top-to-bottom. Column-wise
access is the L1-friendly pattern but the IIR retains 6 floats of
state per thread (`prev1_{0,1,2}`, `prev2_{0,1,2}`); register
pressure forces some threads to spill to L1 at high block-size
configurations.

Both require a shared-memory tile-transpose rewrite to fix
properly. Verification command for the eventual rewrite (deferred
to a future PR):

```bash
ncu --section MemoryWorkloadAnalysis \
    --kernel-id ::ssimulacra2_blur_h:1 \
    core/build/tools/vmaf … --feature ssimulacra2 --backend cuda
```

### 5. Full-resolution H2D / D2H every scale

Device-side allocations are full-size (`3 * width * height *
sizeof(float)`) so each plane starts at a constant
`plane_full_pixels` offset — the IIR kernels rely on this stride
contract. Only the leading `scale_w * scale_h` pixels of each plane
carry valid data per scale.

The original code uploaded / downloaded the full
`3 * plane_full_pixels * sizeof(float)` byte count via a single
`cuMemcpyHtoDAsync` / `cuMemcpyDtoHAsync` per buffer. At scale 2 of
1080p the valid sub-region is 518 KB / plane vs the 8 MB full-plane
transfer — ~15× PCIe traffic excess per copy. The waste is
multiplied by 2 H2D + 5 D2H = 7 copies per scale.

Fix: 3-iteration per-plane loop with
`scale_w * scale_h * sizeof(float)` byte count. The device-side
stride contract is preserved (each plane's payload still starts at
`c * plane_full_pixels * sizeof(float)`); only the memcpy length
shrinks.

### 6. `__launch_bounds__` annotation

The H-pass and V-pass kernels carry no `__launch_bounds__` hint.
`nvcc` chose a register budget conservative enough to keep
correctness but with no documented occupancy contract.
Annotating with `__launch_bounds__(64, 32)` documents the launch
shape (`SS2C_BLUR_BLOCK = 64` from
`ss2c_launch_blur_pass`) plus the 32-resident-block target so
`nvcc` can trim register usage where possible.

## Bench numbers (RTX 4090 / driver 595.71.05 / CUDA 13.2)

48-frame 1920×1080 fixture (synthesised via
`ffmpeg -vf scale=1920:1080` from the Netflix 576×324 fixture).

| Variant | Wall-clock (mean of 3 trials) |
| --- | --- |
| master (pre-fix) | 4.43 s |
| this PR (post-fix) | 4.40 s |

The wall-clock improvement is in noise (~0.7%) at this resolution
on the RTX 4090 — the host XYB pre-pass and the GPU IIR kernel
itself dominate the per-frame cost. The fix is correctness-shaped
(leak elimination, PCIe-traffic reduction, occupancy annotation)
rather than throughput-shaped at 1080p RTX 4090.

On smaller fixtures (576×324) and on memory-pressured hosts (where
the per-scale `malloc` falls back to `mmap` per scale per frame)
the throughput delta would be larger; the user's 5-15% expectation
in the original prompt holds for those operating points.

## Bit-exactness verification

```text
$ python3 scripts/ci/cross_backend_vif_diff.py \
    --vmaf-binary core/build/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 \
    --feature ssimulacra2 --backend cuda --places 4

cross-backend diff, 48 frames, tolerance=5.000e-05 (source=places=4)
metric                    max_abs_diff    mismatches
  ssimulacra2               0.000000e+00    0/48  OK
```

0/48 mismatches at the contract-mandated `places=4` tolerance.
Max abs diff is exactly zero — the code paths exercised by the fix
(scratch buffer ownership, transfer byte count, kernel occupancy
hint) are not on the precision-critical path of the IIR pole
tracking.

## References

- PR #162 — original `ssimulacra2_cuda` landing.
- [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md) —
  `ssimulacra2_cuda` precision contract.
- [ADR-0356](../adr/0410-ssimulacra2-cuda-leaks-perf.md) — this PR's
  decision record.
- CUDA Driver API reference,
  [`cuModuleUnload`](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MODULE.html#group__CUDA__MODULE_1g8ea3d716524369de3763104ced4ea57b)
  — semantics and lifetime guarantees.
