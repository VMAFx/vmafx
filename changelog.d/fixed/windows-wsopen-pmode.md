- Windows: `vmaf_open_utf8()` and libsvm's model writer no longer abort the process
  when a caller passes a POSIX file mode such as `0644`. The change from `_wopen` /
  `_open` to `_wsopen_s` / `_sopen_s` (the CRT's non-deprecated spellings) made the
  CRT reject permission bits other than `_S_IREAD` and `_S_IWRITE` as an invalid
  parameter; the mode is masked to those two bits, as the old calls effectively did.
