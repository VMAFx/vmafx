- **`ffmpeg-patches/0007` libaom-av1 ROI bridge — full impl**:
  patch 0007's libaom-av1 hook is no longer scaffold-only. It now
  caches the parsed `VmafTuneQpFile` in `AOMContext`, allocates a
  segment-id map sized at libaom's mode-info grid
  (`ALIGN_POWER_OF_TWO(dim, 8) >> 2`, since
  `av1/common/enums.h::MI_SIZE == 4`), and on every encoded frame
  picks up to 8 segment QPs from the per-frame qp_offset value
  range (uniform linear binning when the span exceeds
  `AOM_MAX_SEGMENTS == 8`), paints the per-mi segment map by
  expanding each per-16×16-MB qp_offset into a 4×4 block of mi
  cells, and issues `aom_codec_control(&ctx->encoder,
  AOME_SET_ROI_MAP, &roi_map)`. libaom deep-copies the segment
  map and `delta_q[]` table on every control call (see
  `av1/encoder/encoder.c::av1_set_roi_map memcpy`), so a single
  buffer is reused across frames; the qpfile + map are freed in
  `aom_free()`. Smoke:
  `ffmpeg -f lavfi -i testsrc2=size=128x128:r=10:d=0.5
  -c:v libaom-av1 -qpfile clip.qpfile -f null -` against libaom
  v3.13.3 logs `ROI bridge enabled.` and encodes 5 frames clean.
  9/9 patches still apply against pristine n8.1 via
  `git am --3way`. Trade-off: the 8-segment cap rounds nearby
  qp_offsets together (lossy when the saliency model emits more
  than 8 distinct values per frame); finer granularity requires
  driving libaom through its lower-level rate-control plumbing
  (use `vmaf-tune corpus` instead). Retires the libaom-av1
  deferral noted in ADR-0312; no new ADR.
