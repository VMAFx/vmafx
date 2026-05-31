<!-- markdownlint-disable MD060 -->
# Research digest: ADR-0552 — HIP VIF deterministic wavefront reduction

## Problem characterisation

The `integer_vif_hip` horizontal kernel issued one `atomicAdd` per thread (128
per row per field) into seven shared `int64_t` accumulator fields in global
device memory. AMD hardware serialises concurrent `atomicAdd` calls to the same
address as compare-and-swap (CAS) loops. While the final integer sum is
mathematically associative, the CAS retry ordering within a 64-lane wavefront
is non-deterministic in the presence of high contention. This introduced
per-frame per-feature jitter of 0.001–0.014, which the VMAF SVM amplified via
VIF scale coefficients (1.2–2.1 per scale × 4 scales) to approximately 0.031
VMAF-score divergence from CPU — a 200× ADR-0214 gate violation.

## CUDA twin analysis

The CUDA twin (`filter1d.cu`) avoids this with `warp_reduce` in
`cuda_helper.cuh`:

```c
__forceinline__ __device__ int64_t warp_reduce(int64_t x)
{
    for (int i = 16; i > 0; i >>= 1) {
        x += int64_t(__shfl_down_sync(0xffffffff, x & 0xffffffff, i)) |
             int64_t(__shfl_down_sync(0xffffffff, x >> 32, i) << 32);
    }
    return x;
}
```

After reducing, only `threadIdx.x % VMAF_CUDA_THREADS_PER_WARP == 0` issues
the `atomicAdd`. This reduces 128 atomic calls to 4 (one per 32-lane warp) and
makes the result deterministic.

## HIP adaptation

AMD GCN/RDNA default wavefront size = 64 (not 32). Differences from CUDA:

| Feature | CUDA | HIP (AMD) |
|---------|------|-----------|
| Warp/wavefront size | 32 | 64 |
| Reduction intrinsic | `__shfl_down_sync(mask, val, stride)` | `__shfl_xor(val, stride)` |
| Sync mask | Required (`0xffffffff`) | Not needed (wavefront is inherently lock-step) |
| Strides for full reduction | 16, 8, 4, 2, 1 | 32, 16, 8, 4, 2, 1 |

The ported helper `wavefront_reduce_i64` splits the `int64_t` into low/high
32-bit halves, reduces each independently with `__shfl_xor`, then reassembles —
identical in structure to the CUDA twin.

## Wavefront divergence hazard

`__shfl_xor` requires all 64 lanes in the wavefront to execute it at the same
program counter. The original kernels used an early `return` for out-of-bounds
pixels. Early-returning some lanes causes wavefront divergence: the remaining
lanes stall waiting for the diverged lanes to reconverge, and the XOR-shuffle
reads the diverged lanes' register files in an undefined state.

Fix: remove the early return; wrap the computation in `if (x < w && y < h)`;
initialise the accumulator struct to zero. Out-of-bounds threads contribute zero
to the reduction — correct under integer addition.

## Verification

Compilation test: `hipcc --genco --offload-arch=gfx1036` succeeds with no
errors or warnings against the modified kernel.

Numerical verification (to be run in the vmaf-dev-mcp container per the scope
specification):

```bash
docker exec vmaf-dev-mcp /workspace/build-hip/core/tools/vmaf \
  --backend hip \
  --reference /workspace/testdata/ref_576x324_48f.yuv \
  --distorted /workspace/testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --json /tmp/hip.json
# Compare against CPU baseline (VMAF=94.32301); must be within 1e-4
```

## Decision matrix cross-reference

See ADR-0552 `## Alternatives considered` for the full option table.
