- Doc-only sweep of `core/include/libvmaf/`:
  - `libvmaf.h` — corrected the `@param` order on
    `vmaf_score_at_index` to match the function signature
    (`score` before `index`); documented full `cpumask` bit layout
    per architecture (incl. arm64 NEON bit 0 and SVE2 bit 1);
    documented `gpumask` as "any non-zero disables CUDA + SYCL,
    HIP is exempt" per ADR-0530; added a hazard note on
    `VMAF_POOL_METHOD_NB` clarifying it is a count sentinel, not a
    stable API value.
  - `picture.h` — added doxygen field documentation for every
    `VmafPicture` member (pix_fmt, bpc, w[3], h[3], stride[3],
    data[3], ref, priv) and a struct-level brief describing
    ownership transfer semantics.
  - `dnn.h` — deduplicated the `vmaf_dnn_session_attached_ep()`
    stable-string list (was listing `"OpenVINO:CPU"` and
    `"OpenVINO:GPU"` twice).
  No ABI / behaviour change.
