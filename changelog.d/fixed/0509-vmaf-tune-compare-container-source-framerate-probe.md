- `vmaf-tune compare --src container.mp4 ...` now returns sensible
  VMAF scores on container sources whose native rate is not 24 fps.
  Previously the compare CLI threaded the argparse default
  `--framerate=24.0` into `make_bisect_predicate` verbatim; for a
  60 fps source the per-iteration `frame_skip_ref` / `frame_cnt`
  derived from 24 fps indexed the wrong frames in the reference YUV
  (decoded at the container's native 60 fps), mis-aligning reference
  vs. distorted decodes and collapsing the apparent VMAF to the 4-90
  band regardless of CRF — e.g. the dev-mcp BBB 60 fps MP4 reported
  libx264 CRF=6 (near-lossless) at VMAF=90.43, physically impossible.
  `_run_compare` now auto-probes container sources via
  `vmaftune.report.probe_source` and substitutes the probed framerate
  / duration when the user left those flags at their argparse
  defaults; explicit user overrides still win, with a stderr warning
  on probed-vs-user mismatch. New `_TrackedDefaultAction` argparse
  action distinguishes "user explicitly passed 24" from "argparse
  default 24". Sister fix to ADR-0505 (ladder); the compare encode
  plumbing was already correct (bisect already set
  `source_is_container=True`). Verified end-to-end against the
  dev-mcp BBB 60 fps MP4: libx264 CRF=26 → VMAF=94.9 / 528 kbps,
  libx265 CRF=32 → VMAF=93.6 / 365 kbps (ADR-0509, Bug #V7-1).
