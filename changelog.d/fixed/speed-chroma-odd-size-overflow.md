- **`speed_chroma` no longer overruns its frame buffers on odd-sized pictures
  in subsampled formats.** In YUV 4:2:0 and 4:2:2, picture allocators produce
  one extra chroma sample row or column to cover the odd luma extent
  (`vmaf_chroma_extent()`). `speed_chroma` (CPU, CUDA, and HIP) previously
  derived chroma dimensions with integer floor division, under-allocating its
  buffers by one row or column and causing `picture_copy()` to write past the
  buffer under AddressSanitizer. All three extractors now derive chroma extents
  via `speed_chroma_dimensions()`.
