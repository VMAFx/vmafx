- **HIP `init` failure paths reported success after releasing every
  resource (use-after-free), and one of them leaked two device
  buffers.** Three branches across `core/src/feature/hip/integer_adm_hip.c`
  and `core/src/feature/hip/ssimulacra2_hip.c` handed their unwind
  ladder `hipSuccess`. The ladder terminates in a `hipError_t` →
  errno translator that maps `hipSuccess` to `0`, so after tearing
  down the private stream, the events, the HSACO modules and every
  device and pinned allocation, `init` returned `0`;
  `vmaf_feature_extractor_context_init` then set `is_initialized` and
  the first `submit` / `extract` ran against freed device memory.
  Affected `adm_hip` when the feature-name dictionary could not be
  allocated, and `ssimulacra2_hip` on any device- or pinned-allocation
  failure — the latter also discarded the allocator's own error code.
  The ADM path additionally entered the ladder at a tier that skipped
  `d_ref_luma` and `d_dis_luma`, a leak inherited verbatim from a
  pre-HISS-01 `goto fail_host` label placement; because
  `vmaf_feature_extractor_context_close` rejects an uninitialised
  context, `close_fex_hip` never runs after a failed `init`, so
  nothing downstream reclaimed them. All three now unwind the full
  ladder in reverse allocation order and return a negative errno
  (`-ENOMEM` for the ADM dictionary, the allocator's own code for
  SSIMULACRA2), matching every other HIP and Metal extractor. The
  CUDA and SYCL ADM twins were audited and are clean. New
  `fast`-suite regression gates `core/test/test_hip_adm_init_unwind.c`
  and `core/test/test_hip_ssimulacra2_init_unwind.c` drive both
  failure classes with no GPU present (ADR-1296).
