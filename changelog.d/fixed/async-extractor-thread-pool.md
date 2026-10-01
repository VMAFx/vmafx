- **`--threads` no longer breaks GPU twins that are selected by name.**
  `vmaf --backend hip --feature adm_hip --threads N` (and `float_vif_hip`)
  exited with `problem flushing context` for every `N` and wrote no score:
  a twin without a backend flag was handed to the CPU worker pool, whose
  workers call `extract()`, which a `submit()` / `collect()` extractor does
  not have. Such an extractor now runs on the thread that calls
  `vmaf_read_pictures()` whatever its flags, and threaded and unthreaded runs
  give the same scores bit for bit.
