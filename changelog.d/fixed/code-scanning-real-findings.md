- **Five genuine CodeQL findings on the rc.1 train.** `read_luma8()` in
  `core/tools/vmaf_roi_input.h` guarded `shift == 0` after the
  `bitdepth == 8` case had already returned, so the arm was unreachable
  and hid the fact that `shift - 1U` can never underflow; the ternary is
  gone. `check_filtered_center()` and `check_spatial_mask_first_image()`
  in `core/test/test_cambi.c` took `VmafPicture` by value and
  immediately took its address — both now take a pointer.
  `test_vif_lifecycle.c` and `test_vmaf_roi_bounds.c` compared floats
  with `==`; exact equality is the *intent* there (bit-exact frame
  differencing, exactly symmetric saliency), so rather than loosen the
  assertions they now compare bit patterns via a local helper, the same
  shape `test_feature_isa_invariance.c` already documents. The
  remaining CodeQL notes on the train were checked individually and are
  false positives: the `cpp/unused-static-function` hits in
  `pdjson.c`, `test_thread_pool_backpressure.c` and
  `test_fex_ctx_vector.cpp` are all called (some only through a macro or
  through `#define pthread_create observed_create` interposition), and
  the `cpp/constant-comparison` hits in `feature_extractor.cpp` and
  `fex_ctx_vector_internal.h` are `SIZE_MAX`-overflow guards that are
  dead on 64-bit but live on the CI-gated i686 target.
