`vmaf-tune ladder --with-uncertainty` now applies the uncertainty-aware
rung recipe when sampled corpus rows carry `vmaf_interval` payloads,
and uses the active wide-interval threshold as a conservative fallback
for point-only rows.
