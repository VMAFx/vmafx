# fix(hip): register vmaf_fex_integer_motion_hip in extractor list (ADR-0523)

`vmaf_use_feature("motion_hip", NULL)` silently returned an error on every
build because `vmaf_fex_integer_motion_hip` was defined in
`feature/hip/integer_motion_hip.c` but never declared `extern` in nor
registered in `feature_extractor_list[]` in `feature_extractor.c`.
The parity test `test_hip_motion3_parity` (PR #1167) has been hitting this
failure path since it was added.

Fix: add the `extern` declaration and `feature_extractor_list[]` entry inside
`#if HAVE_HIP`, mirroring the CUDA twin (`vmaf_fex_integer_motion_cuda`).
`init()` still returns `-ENOSYS` in scaffold builds — posture unchanged;
only the name lookup is now correct.
