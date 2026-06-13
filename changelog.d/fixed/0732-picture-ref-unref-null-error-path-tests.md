- Add missing error-path unit tests for `vmaf_picture_ref` and
  `vmaf_picture_unref` in `core/test/test_picture.c`: NULL-dst, NULL-src, NULL
  pic, and zeroed-pic (pic->ref == NULL) paths were not exercised by any
  existing C unit test. These cases are now pinned as part of the r12
  error-path coverage audit triggered by ADR-1072 (PREV_REF refcount leak)
  and ADR-1073 (EAGAIN guard fix).
