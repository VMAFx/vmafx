Real on-device GPU kernels for `speed_chroma` and `speed_temporal` on all four
backends (CUDA, SYCL, HIP, Vulkan). Five tile-parallel kernels per frame handle
means, covariance accumulation, independent term, backward substitution, and score;
the serial 25×25 eigendecomposition and QR factorization run on CPU between GPU
passes (the correct algorithmic boundary). The `speed_temporal` variant carries the
`VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag and ping-pong buffers for temporal frame
differences. The Vulkan backend uses a single GLSL compute pipeline with a
push-constant pass selector (7 passes, no per-pass pipeline recompilation).
Parity gate: places=4 vs the CPU reference on the Netflix golden fixture (ADR-0214).
HIP requires `enable_hipcc=true`; without it `init()` returns `-ENOSYS`. (ADR-0567.)
