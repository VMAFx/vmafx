- `vmaf-tune ladder` against a multi-resolution grid now produces
  plausible VMAF scores on cross-resolution rungs. Previously the
  reference leg was decoded at the source's native geometry while
  the rung-target dims (smaller) were handed to the libvmaf CLI for
  both legs; the binary silently mis-parsed the planar bytes and
  emitted ~21 VMAF instead of ~93, which collapsed the post-hull
  ladder to a single rendition. `_maybe_decode_reference` now
  accepts `target_width` / `target_height` and appends a `-vf
  scale=W:H` filter when the rung target differs from the source
  geometry (ADR-0501, Bug #V4-B).
- `vmaf-tune ladder --format json` now emits a top-level
  `samples[]` array carrying every scored `(resolution,
  target_vmaf)` cell pre-hull. `vmaf-tune report --ladder-json`
  reads the array to render the Pareto cloud overlay; previously
  the consumer saw `ladder_samples=0` because the emitter only
  shipped `renditions[]`.
- `vmaf-tune report` distinguishes `encoder unavailable` rows (an
  infrastructure gap — the codec binary is not built into the
  local ffmpeg) from real encode failures. A new `degraded=true`
  field surfaces unavailable rows for dashboards, while `ok=true`
  is preserved whenever every non-`ok` row is unavailable and at
  least one row succeeded (ADR-0501, Bug #V4-C). A new
  `codec_rows_unavailable` counter makes the gap explicit in the
  CLI's stdout JSON.
- Pinned the ADR-0498 strict-mode `--backend NAME` non-zero exit
  contract with a Python integration test that runs the `vmaf`
  binary against the Netflix golden checkerboard pair and asserts
  the refusal byte propagates through `main()` (ADR-0501,
  Bug #V4-A regression guard).
