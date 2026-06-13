Repair the CUDA SpEED TU build: replace legacy `CHECK_CUDA` calls with
`CHECK_CUDA_GOTO`, replace `cuMemAllocHost` with `cuMemHostAlloc` (the
actual `CudaFunctions` table member), and wire `speed_chroma_cuda` +
`speed_temporal_cuda` into meson + the feature-extractor registry.
Adds CPU-vs-CUDA parity tests at places=4 (ADR-0965, closes
T-CUDA-SPEED-TU-REPAIR-2026-05-31).
