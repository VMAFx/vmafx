- **Internal**: name 20+ magic-number literals across fork-added C surfaces
  (CERT INT07-C / MISRA C 4.10 closeout pass 1). Adds `VMAF_MCP_*` constants
  for MCP listener backlog, transport bitmask range, drain cap, SSE
  buffer / poll / scan sizes, UDS path cap, and `compute_vmaf` dimension /
  bit-depth bounds; adds `VMAF_PIC_BPC_{MIN,MAX}` + `VMAF_PIC_DIM_MAX` in
  `core/src/picture.c` and a mirror in `core/src/cuda/picture_cuda.c`; adds
  `VMAF_DNN_NAME_{FALLBACK_BUF,DEDUP_BUF,STRNLEN_CAP}` in `core/src/libvmaf.c`.
  No numeric value changes — bit-exact CPU golden gate preserved. ADR-0874.
