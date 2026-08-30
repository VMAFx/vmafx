- HIP integer VIF extractor no longer crashes with a GPU memory access fault
  when picked by the model-driven dispatch (ADR-0537, closes ADR-0530
  follow-up). Four kernel/host defects fixed: (1) the 4×18 filter table is
  now uploaded to a device buffer at init time (the pre-fix kernel was
  handed a host pointer that the GPU dereferenced); (2) filter half-widths
  corrected from `{9,5,3,0}` to `{8,4,2,1}` (pre-fix read 19 coefficients
  from an 18-entry table); (3) the rd-filter downsample-write path now runs
  so scales 1–3 read the half-resolution planes the previous horizontal pass
  produced (pre-fix left them uninitialised); (4) the host `VmafPicture`
  Y-plane is staged into device memory via `hipMemcpy2DAsync` before the
  scale-0 kernel reads it.  `VMAF_FEATURE_EXTRACTOR_HIP` re-enabled on
  `vmaf_fex_integer_vif_hip`.  End-to-end on AMD gfx1036:
  `vmaf --backend hip --feature vif_hip` returns VMAF VIF scores
  (0.5047 / 0.8764 / 0.9365 / 0.9634 on the Netflix golden 576×324 pair,
  within places=3 of CPU 0.5057 / 0.8791 / 0.9379 / 0.9643 — places=4
  parity tracked as follow-up).
