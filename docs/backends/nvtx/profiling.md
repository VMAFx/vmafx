<!-- markdownlint-disable MD013 MD060 -->
# NVTX Profiling

Build libvmaf with NVTX (NVIDIA Tools Extension) to see per-extractor
ranges in Nsight Systems timelines instead of an opaque `cuLaunchKernel`
wall.

## Build

Run from the repository root. `enable_nvtx` needs the CUDA toolkit headers,
so it must be combined with `enable_cuda`:

```bash
meson setup build core -Denable_cuda=true -Denable_nvtx=true
ninja -C build
```

!!! warning
    `-Denable_nvtx=true` without `-Denable_cuda=true` is a configure error.
    On Windows the option is disabled with a warning. When it is off, the
    NVTX range code is not compiled at all.

## Annotations in-tree

The in-tree ranges use the NVTX C API (`nvtxRangePushA` / `nvtxRangePop`
from `nvtx3/nvToolsExt.h`). They carry no domain, so they appear in the
default NVTX row of the trace.

| File | Range name | What it spans |
|------|------------|---------------|
| [core/src/feature/feature_extractor.cpp](https://github.com/vidiko/vmafx/blob/master/core/src/feature/feature_extractor.cpp) | the extractor name (for example `float_vif_cuda`) | one whole extract call of one feature extractor on one frame |
| [core/src/gpu_picture_pool.cpp](https://github.com/vidiko/vmafx/blob/master/core/src/gpu_picture_pool.cpp) | `fetch idx <pic_idx> <counter>` | taking a picture from the GPU picture pool and its synchronisation callback |

There is one range per extractor, not one per scale. The dispatcher and
drain files under `core/src/cuda/` carry no NVTX calls.

## Running Nsight Systems

```bash
# Full trace, auto-stop when the CLI exits
nsys profile --trace=cuda,nvtx --output=vmaf_trace \
    ./build/tools/vmaf --reference ref.y4m --distorted dis.y4m ...

# Attach GPU metrics (SM active, DRAM bandwidth, PCIe, NVENC/OFA)
nsys profile --trace=cuda,nvtx --gpu-metrics-devices=all \
    --output=vmaf_trace ./build/tools/vmaf ...

# Limit capture to one named range (useful for long sequences)
nsys profile --trace=cuda,nvtx \
    --capture-range=nvtx --nvtx-capture=float_vif_cuda \
    --output=vmaf_trace ./build/tools/vmaf ...

# Textual summary
nsys stats vmaf_trace.nsys-rep
```

Open the `.nsys-rep` in `nsight-sys` (the GUI) to see the timeline. The
extractor ranges appear on the NVTX row; kernel launches sit on the CUDA
HW row below.

## Reading the trace

Useful patterns to look for:

- **Gaps between extractor ranges** — CPU-side stall; commonly I/O from
  `fread` when the input isn't buffered, or FFmpeg demux running
  single-threaded.
- **Kernel row idle while host busy** — the `--threads` setting on the CLI
  is too low, so the dispatcher can't queue enough work to keep the GPU fed.
- **Overlapping copy and kernel rows** — working as designed; the
  submit path overlaps the host-to-device copy of frame N+1 with compute
  for frame N.
- **High DRAM bandwidth but low SM Active** — kernel is memory-bound, not
  compute-bound. Usually the right outcome for VMAF's filter kernels.

## References

- [NVTX C++ API](https://nvidia.github.io/NVTX/doxygen-cpp/index.html)
- [Nsight Systems user
  guide](https://docs.nvidia.com/nsight-systems/UserGuide/index.html)
- [NVIDIA Developer Blog on Nsight +
  NVTX](https://developer.nvidia.com/blog/tag/nsight-systems/)
