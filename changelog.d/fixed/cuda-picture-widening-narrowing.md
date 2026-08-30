- **CUDA `picture_cuda.c` integer-type precision fixes (round-5 clang-tidy
  `bugprone-*` sweep).** Five type-precision defects corrected in
  `core/src/cuda/picture_cuda.c`:
  (1) `vmaf_cuda_picture_download_async` and `vmaf_cuda_picture_upload_async`:
  `CUDA_MEMCPY2D.WidthInBytes` is `size_t` but was computed as a bare
  `unsigned × unsigned` product — for frames wider than ~2 GiB the result
  silently truncated before widening. Fixed by casting the first operand to
  `size_t` (`(size_t)cuda_pic->w[i] * …`).
  (2) `vmaf_cuda_picture_alloc`: same issue in the `cuMemAllocPitch` width
  argument. Fixed identically.
  (3) `vmaf_cuda_picture_alloc_pinned`: `aligned_y` / `aligned_c` were
  `int` but computed from `unsigned` arithmetic; the mask literal
  `~(DATA_ALIGN_PINNED - 1)` evaluated as a signed complement, producing
  UB when the alignment arithmetic exceeded `INT_MAX`. Changed to
  `unsigned` with an explicit `1u` suffix on the mask literal.
  (4) `vmaf_cuda_picture_free`: `vmaf_ref_load()` returns `long` (atomic
  counter); storing the result in `int` causes narrowing. Changed to `long
  err`. Surfaced by `clang-tidy bugprone-narrowing-conversions`.
