## HIP backend: speed_chroma and speed_temporal extractors now reachable

`speed_chroma_hip` and `speed_temporal_hip` (ADR-0567) are now wired into
the HIP build archive and the feature-extractor dispatch table (ADR-0852).
With `enable_hipcc=true` the SpEED on-device GPU kernels run on AMD hardware;
without it `init()` returns `-ENOSYS` (scaffold posture unchanged).
