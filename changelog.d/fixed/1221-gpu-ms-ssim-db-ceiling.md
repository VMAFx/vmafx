- `clip_db` now caps the MS-SSIM dB output at the CPU's
  geometry-derived `max_db` on the CUDA, SYCL and HIP twins. All three
  read it as a clamp on the *linear* score instead — `[0, 1]` then
  `-10*log10(1 - score)` with no ceiling — and none carried a `max_db`
  field. Two consequences: scoring an identical reference/distorted
  pair (MS-SSIM = 1.0) returned **+Inf** where the CPU returns the
  finite `max_db`, and every high-similarity pair returned an unbounded
  dB value, so `clip_db` did not clip. The twins now derive
  `max_db = ceil(10*log10(peak*peak / (0.5/(w*h))))` in `init()` and
  mirror `float_ms_ssim.c::convert_to_db()`, including its
  `score >= 1.0` short-circuit. Re-measure any GPU MS-SSIM dB score
  taken with `clip_db` set. The parity tests could not see this: they
  ran with `NULL` options, and with `enable_db` off neither path
  converts to dB at all. Each backend now has a variant that sets both
  options and feeds an identical pair. See ADR-1221.
