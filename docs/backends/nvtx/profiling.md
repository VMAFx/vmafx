<!-- markdownlint-disable MD013 -->
# NVTX Profiling

NVTX (NVIDIA Tools Extension) annotates ranges in the libvmaf source so
Nsight Systems timelines show per-feature, per-scale boundaries instead
of an opaque `cuLaunchKernel` wall.

## Build

```bash
meson setup build -Denable_cuda=true -Denable_nvtx=true
ninja -C build
```

`enable_nvtx` is only meaningful alongside `enable_cuda`. When off, the
NVTX range macros compile to no-ops.

## Annotations in-tree

The CUDA backend and feature-extractor dispatcher are instrumented with
`nvtx3` C++ ranges:

- [core/src/cuda/dispatch_strategy.c](../../../core/src/cuda/dispatch_strategy.c)
  and [core/src/cuda/drain_batch.c](../../../core/src/cuda/drain_batch.c) —
  per-frame submit/collect boundaries (formerly under the removed
  `ring_buffer.c`).
- [core/src/feature/feature_extractor.c](../../../core/src/feature/feature_extractor.c) —
  one range per `(feature, scale)` pair so you can tell VIF-scale-1 from
  ADM-scale-3 in a timeline.

Each range uses a `libvmaf` domain (`nvtx3::domain{"libvmaf"}`) so you
can filter libvmaf's annotations out from FFmpeg's in the same trace.

## Running Nsight Systems

```bash
# Full trace, auto-stop when the CLI exits
nsys profile --trace=cuda,nvtx --output=vmaf_trace \
    ./build/tools/vmaf --reference ref.y4m --distorted dis.y4m ...

# Attach GPU metrics (SM active, DRAM bandwidth, PCIe, NVENC/OFA)
nsys profile --trace=cuda,nvtx --gpu-metrics-devices=all \
    --output=vmaf_trace ./build/tools/vmaf ...

# Limit capture to a specific range (useful for long sequences)
nsys profile --trace=cuda,nvtx \
    --capture-range=nvtx --nvtx-capture=libvmaf@frame \
    --output=vmaf_trace ./build/tools/vmaf ...

# Textual summary
nsys stats vmaf_trace.nsys-rep
```

Open the `.nsys-rep` in `nsight-sys` (the GUI) to see the timeline. The
`libvmaf` domain appears as its own row; kernel launches sit on the
CUDA HW row below.

## Reading the trace

Useful patterns to look for:

- **Gaps in the libvmaf@frame row** — CPU-side stall; commonly I/O from
  `fread` when the input isn't buffered, or FFmpeg demux running single-threaded.
- **Kernel row idle while host busy** — the `--threads` setting on the CLI
  is too low, so the dispatcher can't queue enough work to keep the GPU fed.
- **Overlapping copy and kernel rows** — working as designed; the
  ring-buffered submit path is overlapping H2D for frame N+1 with compute
  for frame N.
- **High DRAM bandwidth but low SM Active** — kernel is memory-bound, not
  compute-bound. Usually the right outcome for VMAF's filter kernels.

## References

- [NVTX C++ API](https://nvidia.github.io/NVTX/doxygen-cpp/index.html)
- [Nsight Systems user guide](https://docs.nvidia.com/nsight-systems/UserGuide/index.html)
- [NVIDIA Developer Blog on Nsight + NVTX](https://developer.nvidia.com/blog/tag/nsight-systems/)
