- `y4m_input_fetch_frame`: promote `pic_sz` and `c_sz` from `int` to `size_t`
  to eliminate signed-integer overflow for adversarial Y4M dimensions; fix
  `fread(NULL, 1, 0, fp)` UB on no-conversion format paths (ADR-1083).
  Also promotes `_dst` pointer advances in `y4m_convert_42xmpeg2_42xjpeg`,
  `y4m_convert_42xpaldv_42xjpeg`, and `y4m_convert_mono_420jpeg` to `size_t`
  precision (companion to ADR-0977, ADR-1022).
