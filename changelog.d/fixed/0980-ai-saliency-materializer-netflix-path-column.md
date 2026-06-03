# fix(ai): Netflix saliency materializer — root-cause + re-run

Three bugs in `batch_materialize_saliency_features.py` / `materialize_saliency_features.py`
caused 100% failure when run against the Netflix refresh parquet
(`full_features_netflix_refresh_20260520.parquet`, 11 190 rows):

1. **Wrong `path_column`**: manifest defaulted to `"src"` but the parquet uses `"dis_basename"`.
2. **Missing `root`**: `dis_basename` values are relative filenames; `root` was `null`.
3. **Raw YUV not decodable by ffmpeg**: `.yuv` files have no container; ffmpeg needs
   `-f rawvideo -video_size WxH -pix_fmt yuv420p` before `-i`, and ffprobe cannot
   probe dimensions from raw YUV, requiring a `default_width` / `default_height` fallback.

**Fixes applied:**

- `SaliencyMaterializeConfig`: new `default_width` / `default_height` fields (default 0 =
  disabled) used as dimension fallback when the row has no geometry columns and ffprobe
  cannot probe the file.
- `_row_geometry()`: falls back to config defaults when row dimensions are missing.
- `compute_row_saliency()`: prepends `-f rawvideo -video_size WxH -pix_fmt yuv420p` input
  flags for `.yuv` sources before the `-i` argument.
- `materialize_rows()`: in-process per-file saliency cache — avoids re-decoding the same
  source file once per row in per-frame tables. For the Netflix refresh parquet (70 unique
  files × 160 rows each) this reduces runtime from ~289 min to ~1.5 min.
- Diagnostic warnings: emits a stderr warning when `path_column` is absent from the first
  row, and a summary warning when all failures carry `missing-source` status.

Re-run result: 11 190 / 11 190 rows OK, 0 failed, using `saliency_student_v2`.
Output: `.workingdir2/saliency-batch-20260526/out_netflix_v2/full_features_netflix_refresh_20260520.saliency_v2.parquet`
