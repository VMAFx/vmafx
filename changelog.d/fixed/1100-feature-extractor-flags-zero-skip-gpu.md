### Fix GPU-extractor mis-selection when `flags == 0` in feature lookup (ADR-1100)

`vmaf_get_feature_extractor_by_feature_name` had a broken first-pass filter:
`if (flags && !(fex->flags & flags)) continue;` is a no-op when `flags == 0` (the
CPU-only caller path). In an all-backends build, GPU-flagged twins (SYCL, CUDA, HIP)
appear before their CPU counterparts in `feature_extractor_list`, so the SYCL (or
CUDA/HIP) variant was returned. Its `init()` guard (`!fex->sycl_state`) then fired on
every frame, causing `vmaf_read_pictures` to return `-EINVAL` on every frame with
"problem during vmaf_read_pictures". Fix: when `flags == 0`, skip any extractor whose
flags include `VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL |
VMAF_FEATURE_EXTRACTOR_HIP`; all 8 `test_pic_preallocation` sub-tests pass.
