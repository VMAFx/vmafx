# CHUG vmaf column: compute via vmaf_v0.6.1 SDR baseline (supersedes PR #898)

The `vmaf` column in CHUG/K150K feature parquets was always NaN because the
libvmaf CLI invocation did not include a `--model` argument.  This PR implements
Option B (per user direction 2026-05-16, Research-0135): add
`--model version=vmaf_v0.6.1` to both the CUDA and CPU invocations in
`_run_feature_passes`, causing libvmaf to dispatch the composite model and emit a
per-frame `vmaf` key.

**Caveat:** `vmaf_v0.6.1` is SDR-trained; scores on PQ HDR clips are mis-calibrated
in absolute terms but remain valid for relative bitrate-ladder ranking within a
content group.  Replace the model arg when the Netflix HDR vmaf model ships.

Supersedes PR #898 (Option A — drop the column).
