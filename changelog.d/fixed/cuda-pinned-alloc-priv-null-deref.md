- **`vmaf_cuda_picture_alloc_pinned`: null-deref when `vmaf_picture_priv_init` fails
  (cross-PR seam, CWE-476, round-6 audit)** — `vmaf_picture_priv_init` allocates
  `pic->priv`; if it fails (OOM), the function returned `NULL` in `pic->priv`. The
  caller then immediately dereferenced `pic->priv` to write CUDA state fields
  (`priv->cuda.state`, `priv->cuda.ctx`), and also passed the picture to
  `vmaf_picture_set_release_callback`, which unconditionally dereferences `priv->cookie`.
  The `|=` idiom (`err |= vmaf_picture_priv_init(pic)`) evaluates the right-hand side
  unconditionally regardless of prior failure — the analogous bug in `picture.c` was
  fixed by PR #700 (CWE-476), but the identical pattern in `picture_cuda.c` was missed.
  Fix: replace `|=` with sequential checks mirroring the PR #700 pattern.
  Secondary fix: `DATA_ALIGN_PINNED - 1` in the alignment expressions (lines 135–136)
  changed to `DATA_ALIGN_PINNED - 1u` to match the fully-unsigned pattern that PR #708
  applied to `picture.c`.
