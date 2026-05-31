- Public C-API headers under `core/include/libvmaf/` now use the
  ISO-compliant `LIBVMAF_<BASENAME>_H` include-guard pattern instead of
  the SEI CERT DCL37-C-violating `__VMAF_*__` form. Clang ≥ 13's
  `-Wreserved-identifier` (and `-Werror` builds on clang 22) previously
  rejected the headers; downstream consumers can now build the fork's
  public surface under strict-mode flags. Nine headers renamed
  (`libvmaf.h`, `picture.h`, `feature.h`, `model.h`, `macros.h`,
  `vmaf_assert.h`, `dnn.h`, `libvmaf_cuda.h`, `libvmaf_sycl.h`); no ABI
  change — only the guard macro symbols change, and a repository-wide
  grep confirmed no source file consumes any of the old guard symbols.
  ADR-0972, Round 27 audit A.1.
