**fix(picture-pool):** eliminate use-after-free window in synchronous
`prev_ref` dispatch (`read_pictures_dispatch_one`) and fix buffer leak
in `pool_preallocate_pictures` error unwind (ADR-0778).

- `core/src/libvmaf.c`: replace bare struct copy of `vmaf->prev_ref`
  with `vmaf_picture_ref` + matching `vmaf_picture_unref`, preventing
  the pool from reusing the buffer while an extractor reads it.
- `core/src/picture_pool.c`: restructure `pool_preallocate_pictures` so
  `priv`/`ref` are stripped only after the full allocation loop succeeds;
  error unwind now calls `vmaf_picture_unref` on intact pictures instead
  of silently leaking their buffers.
