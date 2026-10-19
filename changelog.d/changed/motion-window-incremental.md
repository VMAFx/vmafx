- **`motion2` / `motion3` are final one frame after their frame, not at the
  flush (RC4, ADR-2090).** The integer motion extractors (`motion`,
  `motion_v2`) and every GPU twin that derived `motion2` / `motion3` at the
  end of the stream now write a frame's scores as soon as the frame after it
  is scored (frame 2 for frames 0 and 1 with `motion_five_frame_window`); the
  last frame's scores still come with the flush. The values are unchanged,
  bit for bit: the derivation runs the same statements in the same order and
  carries the moving average from frame to frame. A per-frame model score, a
  metadata callback for a model, `vmaf_score_pooled()` over the frames read
  so far, and a VMAFx window over a VMAF model are therefore available while
  the stream runs; on CUDA, `motion_cuda` completes frames per readback
  batch of eight. See [motion](docs/metrics/motion.md#when-motion2-and-motion3-are-final).
