- **`motion_v2` mirror off-by-one across CPU + AVX2 + AVX-512 + NEON.**
  Cherry-picks Netflix/vmaf upstream `856d3835` "libvmaf/motion_v2:
  fix mirroring behavior, since a44e5e61". The `mirror()` helper for
  vertical-edge pixel reflection clamps `idx >= size` to
  `2*size - idx - 2` (was `-1`), matching the v1 motion extractor's
  corrected behaviour. The fork's NEON twin (ADR-0145) is updated in
  lockstep so the SIMD-vs-scalar bit-equality contract holds. GPU
  twins (CUDA, SYCL, HIP, Vulkan) of `motion_v2` retain their
  existing mirror formula for now; their refresh is tracked in
  `docs/rebase-notes.md` §0337.
