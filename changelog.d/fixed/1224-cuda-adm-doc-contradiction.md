- `docs/backends/cuda/overview.md` contradicted itself about the default
  model's ADM: one bullet said `integer_adm_cuda` lacks `adm_csf_mode: 2`
  and that libvmaf falls back to the CPU extractor, while a later section
  dated the same day said the whole ADM family runs on the device. The
  code and a live run agree with the later section —
  `integer_adm_cuda.c:840` declares `adm_csf_mode`,
  `T-GPU-ADM-CSF-MODE-NOT-PORTED-2026-09-05` is closed, and a CUDA run of
  the default model emits no CPU-fallback notice. The stale bullet is
  corrected. See ADR-1224.
