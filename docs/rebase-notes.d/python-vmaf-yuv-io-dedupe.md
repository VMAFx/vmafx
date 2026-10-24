## python-vmaf YUV reader and writer share one base (2026-10-24)

- `compat/python-vmaf/tools/reader.py` and `writer.py` mirror Netflix's
  `python/vmaf/tools/reader.py` and `writer.py`. The fork moved their format
  tables, file open and close, and the bit-depth helpers into `_YuvIoBase`
  in `reader.py`, which `YuvWriter` imports. When an upstream sync changes
  a format list or the `__init__` of either class, apply the change to
  `_BASE_UV_MULTIPLIERS`, `_YuvIoBase` or the subclass suffix lists. Do not
  restore the two full copies.
- `decorator.py` builds every memoization key with `_cache_key()`, and
  `plot.py` selects slices with `_extract_slice_data()`. Upstream changes
  to the key or the slice selection go into those helpers.
