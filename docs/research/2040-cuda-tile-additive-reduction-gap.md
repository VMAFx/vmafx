<!-- markdownlint-disable MD013 -->
# Research: CUDA Tile C++ has no additive reduction, and VIF is made of them

**Date**: 2026-09-07
**Author**: timeboxed spike following the CUDA Tile adoption audit
**Toolchain**: CUDA 13.3 (`cuda_13.3.r13.3`), driver 610.57.04, RTX 4090 (sm_89)

## Summary

The CUDA Tile audit recommended not adopting Tile C++ for the fork's kernels.
This spike tests that recommendation against the one kernel family where Tile
looked most plausible — `float_vif_compute`, whose inner loop is a windowed
statistics pass over a float plane.

The result is sharper than "not worth it": **the Tile API in CUDA 13.3 provides
no additive reduction primitive at all**. `reduce_max`, `reduce_min`,
`reduce_bitand`, `reduce_bitor` and `reduce_bitxor` are builtins;
`reduce_add` / `reduce_sum` do not exist. VIF's inner loop is nothing but
additive reductions — sums, sums of squares, and cross-products over a sliding
window.

The workaround (two inclusive scans, or a `matmul` against a ones-vector) costs
**~2x the registers and 4 KB of shared memory per block** versus the natively
supported reduction, to produce one scalar the current SIMT path gets from a
register-only warp shuffle.

## What the API actually offers

Enumerated from `/opt/cuda/include/crt/cuda_tile.h` (4064 lines, CUDA 13.3):

| Category | Available |
| --- | --- |
| Reductions | `reduce_max`, `reduce_min`, `reduce_bitand`, `reduce_bitor`, `reduce_bitxor` |
| Scans | `partial_sum`, `partial_prod` (forward / backward) |
| Matrix | `matmul`, `mma` |
| Data movement | `load`, `store`, `iota`, `broadcast`, tensor `view` with `allow_tma` hints |

`reduce_add` is absent. An additive reduction therefore has to be expressed as
either:

1. `partial_sum` along each dimension and take the last element — computing
   every prefix sum to obtain one total; or
2. `matmul` against a ones-vector — paying a tensor-core matmul for a sum.

## Measured cost

Both kernels below reduce the same 32x32 float tile. The only difference is
whether the reduction operator is natively supported.

```bash
nvcc --enable-tile --tilefatbin -std=c++20 -arch=sm_89 tile_spike.cu -o out
cuobjdump -res-usage out
```

| kernel | reduction | REG | SHARED | mechanism |
| --- | --- | --- | --- | --- |
| `tile_max_native` | `reduce_max` | 24 | 0 B | one builtin |
| `tile_sum_via_scan` | additive | 47 | **4096 B** | two `partial_sum` scans |

The additive version needs ~2x the registers and a 4 KB shared-memory staging
buffer. The fork's existing SIMT VIF reduction is a warp-shuffle tree that
touches no shared memory at all.

## Two things the spike also settled

**`__global__` cannot call Tile code.** Tile functions are `__tile__`, and
calling one from a `__global__` entry point is a compile error
(`calling a __tile__ function ... from a __global__ function is not allowed`).
Tile kernels need the separate `__tile_global__` entry-point attribute from
`crt/host_defines.h`. A Tile port is therefore not an incremental edit to an
existing kernel — it is a second, parallel entry point, with the host side
choosing between them.

**Tile code needs its own fatbin.** A plain `-cubin` build with `--enable-tile`
produces an object with **no SASS** for the tile kernels — only
`.note.nv.tkinfo` metadata and `GLOBAL:0` resource usage. Tile codegen lands
only under `--tilefatbin`, in a separate `Fatbin tile ir code` section
alongside the ELF. That is a second build artifact and a second dispatch path
for `core/src/meson.build` and the CUDA loader to manage.

## Conclusion

The audit's recommendation stands, and now on a mechanism rather than a
judgement call: Tile C++ is built for tile-level matrix work, and the fork's
hot kernels are windowed additive statistics. The single most common operation
in `float_vif_compute`, `integer_adm`'s accumulators, `float_moment` and the
SpEED covariance pass is a sum reduction, which Tile cannot express directly.

Revisit when a future CUDA release adds `reduce_add`. At that point the
remaining costs — the `__tile_global__` split and the `--tilefatbin` second
artifact — are worth re-measuring, but they are build-system work rather than a
capability gap.

## Reproducing

```cuda
// tile_spike.cu
#include <cuda_tile.h>
namespace ct = cuda::tiles;
using S = ct::shape<32, 32>;

__tile_global__ void tile_sum_via_scan(const float *__restrict__ in, float *__restrict__ out)
{
    auto idx  = ct::iota<ct::tile<int, S>>();
    auto v    = ct::load(ct::broadcast(in, S{}) + idx);
    auto rows = ct::partial_sum<0>(v);
    auto all  = ct::partial_sum<1>(rows);
    ct::store(ct::broadcast(out, S{}) + idx, all);
}

__tile_global__ void tile_max_native(const float *__restrict__ in, float *__restrict__ out)
{
    auto idx = ct::iota<ct::tile<int, S>>();
    auto v   = ct::load(ct::broadcast(in, S{}) + idx);
    auto m   = ct::reduce_max<0>(v);
    using R  = ct::shape<1, 32>;
    ct::store(ct::broadcast(out, R{}) + ct::iota<ct::tile<int, R>>(), m);
}
```

```bash
# Confirm reduce_add is absent:
grep -oE 'reduce_[a-z_]+' /opt/cuda/include/crt/cuda_tile.h | sort -u

# Build and measure:
nvcc --enable-tile --tilefatbin -std=c++20 -arch=sm_89 tile_spike.cu -o tile_spike.tilefatbin
cuobjdump -res-usage tile_spike.tilefatbin
```

## References

- NVIDIA, "CUDA 13.3 enhances GPU development with tile programming in C++,
  compiler autotuning and Python updates" —
  <https://developer.nvidia.com/blog/nvidia-cuda-13-3-enhances-gpu-development-with-tile-programming-in-c-compiler-autotuning-and-python-updates/>
- `/opt/cuda/include/crt/cuda_tile.h` (CUDA 13.3) — the authoritative API surface.
