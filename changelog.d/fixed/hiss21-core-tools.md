- **`vmaf-perShot` and the VPL decode tool can no longer spin forever on an
  input that never ends.** The per-shot scan now stops at
  `VMAF_PER_SHOT_MAX_FRAMES` and reports `-EFBIG` instead of wrapping its
  `uint32_t` frame counter, and `vmaf_vpl`'s decode retry loop gives up after
  the same 60 s ceiling its sync operation already used rather than retrying a
  wedged device indefinitely. Both previously used an unbounded `for (;;)`.
- **Core CLI cleanup paths restructured to satisfy the HISS control-flow
  contract.** The YUV reader, the VPL tool, the per-shot scanner and the
  Windows `getopt` shim drop twelve `goto` jumps and six oversized functions
  in favour of staged helpers. Scores, output text, exit codes and
  freed-resource ordering are unchanged — the golden 576x324 pair still scores
  76.667831 and `vmaf-perShot` still emits a byte-identical plan.
