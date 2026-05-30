- `VMAF_<BACKEND>_DISPATCH` strategy-name matcher now requires a
  token boundary (`'\0'`, `','`, `'\n'`, space, or tab) after the
  matched strategy name. Previously the `strncmp(v, name, slen)`
  check in `core/src/gpu_dispatch_parse.h` returned a match for
  any string with the strategy name as a prefix, so a typo like
  `VMAF_CUDA_DISPATCH=feature:directx` silently routed to the
  `direct` strategy instead of being treated as unknown. Adds
  `core/test/test_gpu_dispatch_parse.c` to pin the strict-match
  contract for both prefix-collision cases (`directx`,
  `balancedx`) and valid terminators (`,`, `\n`, end-of-string).
