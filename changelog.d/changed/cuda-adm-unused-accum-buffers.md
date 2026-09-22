- **The CUDA integer ADM extractor allocates about 50 MB less device memory
  at 1080p.** Two scratch buffers sized to the frame (`tmp_accum`,
  3 x w x h x 8 bytes, and `tmp_accum_h`) were allocated at init and passed to
  kernels that never read them, a leftover from an earlier reduction scheme.
  They are gone; scores are unchanged.
