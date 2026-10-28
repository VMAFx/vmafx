## Picture conversion DEBUG lines, Netflix/vmaf PR 1710 (2026-10-27)

- `core/src/conversion_context.{c,h}`: `logged_no_conversion`, one
  `none needed` line per conversion state. `core/src/picture_convert.c`:
  `log_conversion()` and the colour name helpers (zimg builds only).
  `core/test/test_picture_convert_log.c` checks both lines and their level.
- Ported from the open Netflix/vmaf PR 1710 before it landed; the fork logs
  from its own conversion state, not from upstream's `libvmaf.c`.
- **On sync**: when PR 1710 lands upstream, take upstream's message wording,
  keep the once-per-state flag and the test, and move the test's expected
  prefixes to the new wording.
