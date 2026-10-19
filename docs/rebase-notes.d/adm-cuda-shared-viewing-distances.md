## ADM second viewing distance on CUDA, shared merge helpers (2026-10-08)

- `core/src/feature/adm_view_dist.{c,h}` (new): the merge callback and the
  second distance's names, read through the option table by name, for every
  ADM descriptor. `integer_adm.c` drops its own copies; `adm_cuda` sets the
  same hooks. Fork-only, nothing upstream to merge.
- `core/src/feature/cuda/integer_adm_cuda.c`: `adm_scale0_device()` /
  `adm_scale123_device()` became `*_transform()` (DWT) and `*_weigh()`
  (denominator, CSF, CM, AIM at one distance); `tmp_res` and `results_host`
  hold one result block per distance; the host conclusion takes the distance
  as an argument. **On sync**: an upstream change to the CUDA ADM driver lands
  in the matching half; keep the per-distance loop.
- `core/test/test_cuda_adm_parity.c`: the recorded option gap is gone;
  `test_adm_two_views_exact` and `test_adm_merged_registrations_exact` hold
  both distances to the CPU's bits.
