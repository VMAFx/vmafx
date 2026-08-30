- **`vmaf_hip_import_state` returned `-ENOSYS`, breaking
  `vmaf --backend hip` on every AMD ROCm host.** The
  library-side state-binding stub in `core/src/hip/common.c`
  returned `-ENOSYS` with a stale comment promising a follow-up
  PR that had since landed (ADR-0468 added the first real HIP
  feature kernel, `float_adm_hip`, months earlier). The CLI's
  `--backend hip` path constructed a `VmafHipState` successfully,
  then called `vmaf_hip_import_state(vmaf, hip_state)` which
  immediately bailed; the CLI emitted "problem during
  vmaf_hip_import_state" and aborted with exit 255 — even on a
  fully healthy AMD gfx1036 host with ROCm 6.4 (HIP state init,
  stream create, device probe all succeeded under
  `AMD_LOG_LEVEL=3`; only the library bind was missing).
  Fix moves the function from `core/src/hip/common.c` into
  `core/src/libvmaf.c` and implements it as a SYCL / Vulkan /
  Metal-style "stash the borrowed state pointer on the
  VmafContext and return 0" wrapper. Verified end-to-end against
  the Netflix golden src01 pair inside the `vmaf-dev-mcp`
  container: VMAF = 76.66783, bit-exact match against the CPU
  backend (CPU = 76.66783, delta = 0). HIP joins CUDA / SYCL /
  Vulkan within the `places=4` (1e-4) cross-backend gate
  (ADR-0214). The HIP-flagged extractors keep the
  `VMAF_FEATURE_EXTRACTOR_HIP` bit cleared for now (dispatch
  routes through their CPU twins, which is why scores match CPU
  exactly); promoting the flag bit + adding the
  `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` plumbing is a separate
  follow-up. Closes `docs/state.md` row
  `T-HIP-IMPORT-STATE-ENOSYS-2026-05-18`; ADR-0519; follows on
  from ADR-0514.
